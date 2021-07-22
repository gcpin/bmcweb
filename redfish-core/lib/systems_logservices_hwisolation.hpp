#pragma once

#include "bmcweb_config.h"
#include "app.hpp"
#include "async_resp.hpp"
#include "http_request.hpp"
#include "registries/privilege_registry.hpp"
#include "utils/name_utils.hpp"
#include "utils/time_utils.hpp"

#include <boost/beast/http/verb.hpp>
#include <boost/url/format.hpp>

#include <memory>
#include <string>
#include <string_view>

namespace redfish
{
/****************************************************
 * Redfish HardwareIsolationLog interfaces
 ******************************************************/

using RedfishResourceDBusInterfaces = std::string;
using RedfishResourceCollectionUri = std::string;
using RedfishUriListType = std::unordered_map<RedfishResourceDBusInterfaces,
                                              RedfishResourceCollectionUri>;

static const RedfishUriListType redfishUriList = {
    {"xyz.openbmc_project.Inventory.Item.Cpu",
     "/redfish/v1/Systems/system/Processors"},
    {"xyz.openbmc_project.Inventory.Item.Dimm",
     "/redfish/v1/Systems/system/Memory"},
    {"xyz.openbmc_project.Inventory.Item.CpuCore",
     "/redfish/v1/Systems/system/Processors/<str>/SubProcessors"}};

using AssociationsValType =
    std::vector<std::tuple<std::string, std::string, std::string>>;
using GetManagedPropertyType = boost::container::flat_map<
    std::string,
    std::variant<std::string, bool, uint8_t, int16_t, uint16_t, int32_t,
                 uint32_t, int64_t, uint64_t, double, AssociationsValType>>;

using GetManagedObjectsType = boost::container::flat_map<
    sdbusplus::message::object_path,
    boost::container::flat_map<std::string, GetManagedPropertyType>>;

/**
 * @brief API Used to add the supported HardwareIsolation LogServices Members
 *
 * @param[in] req - The HardwareIsolation redfish request (unused now).
 * @param[in] asyncResp - The redfish response to return.
 *
 * @return The redfish response in the given buffer.
 */
inline void getSystemHardwareIsolationLogService(
    const crow::Request& /* req */,
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp)
{
    asyncResp->res.jsonValue["@odata.id"] = boost::urls::format(
        "/redfish/v1/Systems/{}/LogServices/HardwareIsolation",
        BMCWEB_REDFISH_SYSTEM_URI_NAME);
    asyncResp->res.jsonValue["@odata.type"] = "#LogService.v1_2_0.LogService";
    asyncResp->res.jsonValue["Name"] = "Hardware Isolation LogService";
    asyncResp->res.jsonValue["Description"] =
        "Hardware Isolation LogService for system owned devices";
    asyncResp->res.jsonValue["Id"] = "HardwareIsolation";

    asyncResp->res.jsonValue["Entries"]["@odata.id"] = boost::urls::format(
        "/redfish/v1/Systems/{}/LogServices/HardwareIsolation/Entries",
        BMCWEB_REDFISH_SYSTEM_URI_NAME);

    asyncResp->res.jsonValue["Actions"]["#LogService.ClearLog"]
                            ["target"] = boost::urls::format(
        "/redfish/v1/Systems/{}/LogServices/HardwareIsolation/Actions/LogService.ClearLog",
        BMCWEB_REDFISH_SYSTEM_URI_NAME);
}

/**
 * @brief Workaround to handle DCM (Dual-Chip Module) package for Redfish
 *
 * This API will make sure processor modeled as dual chip module, If yes then,
 * replace the redfish processor id as "dcmN-cpuN" because redfish currently
 * does not support chip module concept.
 *
 * @param[in] dbusObjPath - The D-Bus object path to return the object instance
 *
 * @return the object instance with it parent instance id if the given object
 *         is a processor else the object instance alone.
 */
inline std::string getIsolatedHwItemId(
    const sdbusplus::message::object_path& dbusObjPath)
{
    std::string isolatedHwItemId;

    if ((dbusObjPath.filename().find("cpu") != std::string::npos) &&
        (dbusObjPath.parent_path().filename().find("dcm") != std::string::npos))
    {
        isolatedHwItemId =
            std::format("{}-{}", dbusObjPath.parent_path().filename(),
                        dbusObjPath.filename());
    }
    else
    {
        isolatedHwItemId = dbusObjPath.filename();
    }
    return isolatedHwItemId;
}

/**
 * @brief API used to get redfish uri of the given dbus object and fill into
 *        "OriginOfCondition" property of LogEntry schema.
 *
 * @param[in] asyncResp - The redfish response to return.
 * @param[in] dbusObjPath - The DBus object path which represents redfishUri.
 * @param[in] entryJsonIdx - The json entry index to add isolated hardware
 *                            details in the appropriate entry json object.
 *
 * @return The redfish response with "OriginOfCondition" property of
 *         LogEntry schema if success else return the error
 */
inline void getRedfishUriByDbusObjPath(
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const sdbusplus::message::object_path& dbusObjPath,
    const size_t entryJsonIdx)
{
    crow::connections::systemBus->async_method_call(
        [asyncResp, dbusObjPath,
         entryJsonIdx](const boost::system::error_code& ec,
                       const dbus::utility::MapperGetObject& objType) {
            if (ec || objType.empty())
            {
                BMCWEB_LOG_ERROR(
                    "DBUS response error [{} : {}] when tried to get the RedfishURI of isolated hareware: {}",
                    ec.value(), ec.message(), dbusObjPath.str);
                messages::internalError(asyncResp->res);
                return;
            }

            RedfishUriListType::const_iterator redfishUriIt;
            for (const auto& service : objType)
            {
                for (const auto& interface : service.second)
                {
                    redfishUriIt = redfishUriList.find(interface);
                    if (redfishUriIt != redfishUriList.end())
                    {
                        // Found the Redfish URI of the isolated hardware unit.
                        break;
                    }
                }
                if (redfishUriIt != redfishUriList.end())
                {
                    // No need to check in the next service interface list
                    break;
                }
            }

            if (redfishUriIt == redfishUriList.end())
            {
                BMCWEB_LOG_ERROR(
                    "The object[{}] interface is not found in the Redfish URI list. Please add the respective D-Bus interface name",
                    dbusObjPath.str);
                messages::internalError(asyncResp->res);
                return;
            }

            // Fill the isolated hardware object id along with the Redfish URI
            std::string redfishUri =
                redfishUriIt->second + "/" + getIsolatedHwItemId(dbusObjPath);

            // Make sure whether no need to fill the parent object id in the
            // isolated hardware Redfish URI.
            const std::string uriIdPattern{"<str>"};
            size_t uriIdPos = redfishUri.rfind(uriIdPattern);
            if (uriIdPos == std::string::npos)
            {
                if (entryJsonIdx > 0)
                {
                    asyncResp->res.jsonValue["Members"][entryJsonIdx - 1]
                                            ["Links"]["OriginOfCondition"] = {
                        {"@odata.id", redfishUri}};
                }
                else
                {
                    asyncResp->res.jsonValue["Links"]["OriginOfCondition"] = {
                        {"@odata.id", redfishUri}};
                }
                return;
            }

            // Fill the all parents Redfish URI id.
            // For example, the processors id for the core.
            // "/redfish/v1/Systems/system/Processors/<str>/SubProcessors/core0"
            crow::connections::systemBus->async_method_call(
                [asyncResp, dbusObjPath, entryJsonIdx, redfishUri, uriIdPos,
                 uriIdPattern](const boost::system::error_code ec1,
                               const boost::container::flat_map<
                                   std::string,
                                   boost::container::flat_map<
                                       std::string, std::vector<std::string>>>&
                                   subtree) mutable {
                    if (ec1)
                    {
                        BMCWEB_LOG_ERROR(
                            "DBUS response error [{} : {}] when tried to fill the parent objects id in the RedfishURI: {} of the isolated hareware: {}",
                            ec1.value(), ec1.message(), redfishUri,
                            dbusObjPath.str);

                        messages::internalError(asyncResp->res);
                        return;
                    }

                    while (uriIdPos != std::string::npos)
                    {
                        std::string parentRedfishUri =
                            redfishUri.substr(0, uriIdPos - 1);
                        RedfishUriListType::const_iterator parentRedfishUriIt =
                            std::find_if(
                                redfishUriList.begin(), redfishUriList.end(),
                                [parentRedfishUri](const auto& ele) {
                                    return parentRedfishUri == ele.second;
                                });

                        if (parentRedfishUriIt == redfishUriList.end())
                        {
                            BMCWEB_LOG_ERROR(
                                "Failed to fill Links:OriginOfCondition because unable to get parent Redfish URI [{}] DBus interface for the identified Redfish URI: {} of the given DBus object path: {} ",
                                parentRedfishUri, redfishUri, dbusObjPath.str);
                            messages::internalError(asyncResp->res);
                            return;
                        }

                        std::string parentObj;
                        for (const auto& obj : subtree)
                        {
                            if (dbusObjPath.str.find(
                                    sdbusplus::message::object_path(obj.first)
                                        .filename()) == std::string::npos)
                            {
                                // The object is not found in the isolated
                                // hardware object path
                                continue;
                            }

                            for (const auto& service : obj.second)
                            {
                                for (const auto& interface : service.second)
                                {
                                    if (interface == parentRedfishUriIt->first)
                                    {
                                        parentObj = obj.first;
                                        break;
                                    }
                                }
                                if (!parentObj.empty())
                                {
                                    break;
                                }
                            }
                            if (!parentObj.empty())
                            {
                                break;
                            }
                        }

                        if (parentObj.empty())
                        {
                            BMCWEB_LOG_ERROR(
                                "Failed to fill Links:OriginOfCondition because unable to get parent DBus path for the identified parent Redfish URI: {} of the given DBus object path: {}",
                                parentRedfishUri, dbusObjPath.str);

                            messages::internalError(asyncResp->res);
                            return;
                        }

                        redfishUri.replace(
                            uriIdPos, uriIdPattern.length(),
                            getIsolatedHwItemId(
                                sdbusplus::message::object_path(parentObj)));

                        uriIdPos = redfishUri.rfind(uriIdPattern);
                    }

                    if (entryJsonIdx > 0)
                    {
                        asyncResp->res.jsonValue["Members"][entryJsonIdx - 1]
                                                ["Links"]["OriginOfCondition"] =
                            {{"@odata.id", redfishUri}};
                    }
                    else
                    {
                        asyncResp->res.jsonValue["Links"]["OriginOfCondition"] =
                            {{"@odata.id", redfishUri}};
                    }
                },
                "xyz.openbmc_project.ObjectMapper",
                "/xyz/openbmc_project/object_mapper",
                "xyz.openbmc_project.ObjectMapper", "GetSubTree",
                "/xyz/openbmc_project/inventory", 0,
                std::array<const char*, 0>{});
        },
        "xyz.openbmc_project.ObjectMapper",
        "/xyz/openbmc_project/object_mapper",
        "xyz.openbmc_project.ObjectMapper", "GetObject", dbusObjPath.str,
        std::array<const char*, 0>{});
}

/**
 * @brief API used to get "PrettyName" by using the given dbus object path
 *        and fill into "Message" property of LogEntry schema.
 *
 * @param[in] asyncResp - The redfish response to return.
 * @param[in] dbusObjPath - The DBus object path which represents redfishUri.
 * @param[in] entryJsonIdx - The json entry index to add isolated hardware
 *                            details in the appropriate entry json object.
 *
 * @return The redfish response with "Message" property of LogEntry schema
 *         if success else nothing in redfish response.
 */

inline void getPrettyNameByDbusObjPath(
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const sdbusplus::message::object_path& dbusObjPath,
    const size_t entryJsonIdx)
{
    crow::connections::systemBus->async_method_call(
        [asyncResp, dbusObjPath,
         entryJsonIdx](const boost::system::error_code ec,
                       const dbus::utility::MapperGetObject& objType) mutable {
            if (ec || objType.empty())
            {
                BMCWEB_LOG_ERROR(
                    "DBUS response error [{} : {}] when tried to get the dbus name of isolated hareware: {}",
                    ec.value(), ec.message(), dbusObjPath.str);
                messages::internalError(asyncResp->res);
                return;
            }

            if (objType.size() > 1)
            {
                BMCWEB_LOG_ERROR(
                    "More than one dbus service implemented the xyz.openbmc_project.Inventory.Item interface to get the PrettyName");
                messages::internalError(asyncResp->res);
                return;
            }

            if (objType[0].first.empty())
            {
                BMCWEB_LOG_ERROR(
                    "The retrieved dbus name is empty for the given dbus object: {}",
                    dbusObjPath.str);
                messages::internalError(asyncResp->res);
                return;
            }

            if (entryJsonIdx > 0)
            {
                asyncResp->res
                    .jsonValue["Members"][entryJsonIdx - 1]["Message"] =
                    dbusObjPath.filename();
                auto msgPropPath = "/Members"_json_pointer;
                msgPropPath /= entryJsonIdx - 1;
                msgPropPath /= "Message";
                name_util::getPrettyName(asyncResp, dbusObjPath.str, objType,
                                         msgPropPath);
            }
            else
            {
                asyncResp->res.jsonValue["Message"] = dbusObjPath.filename();
                name_util::getPrettyName(asyncResp, dbusObjPath.str, objType,
                                         "/Message"_json_pointer);
            }
        },
        "xyz.openbmc_project.ObjectMapper",
        "/xyz/openbmc_project/object_mapper",
        "xyz.openbmc_project.ObjectMapper", "GetObject", dbusObjPath.str,
        std::array<const char*, 1>{"xyz.openbmc_project.Inventory.Item"});
}

/**
 * @brief API used to fill the isolated hardware details into LogEntry schema
 *        by using the given isolated dbus object which is present in
 *        xyz.openbmc_project.Association.Definitions::Associations of the
 *        HardwareIsolation dbus entry object.
 *
 * @param[in] asyncResp - The redfish response to return.
 * @param[in] dbusObjPath - The DBus object path which represents redfishUri.
 * @param[in] entryJsonIdx - The json entry index to add isolated hardware
 *                            details in the appropriate entry json object.
 *
 * @return The redfish response with appropriate redfish properties of the
 *         isolated hardware details into LogEntry schema if success else
 *         nothing in redfish response.
 */
inline void fillIsolatedHwDetailsByObjPath(
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const sdbusplus::message::object_path& dbusObjPath,
    const size_t entryJsonIdx)
{
    // Fill Redfish uri of isolated hardware into "OriginOfCondition"
    if (dbusObjPath.filename().find("unit") != std::string::npos)
    {
        // If Isolated Hardware object name contain "unit" then that unit
        // is not modelled in inventory and redfish so the "OriginOfCondition"
        // should filled with it's parent (aka FRU of unit) path.
        getRedfishUriByDbusObjPath(asyncResp, dbusObjPath.parent_path(),
                                   entryJsonIdx);
    }
    else
    {
        getRedfishUriByDbusObjPath(asyncResp, dbusObjPath, entryJsonIdx);
    }

    // Fill PrettyName of isolated hardware into "Message"
    getPrettyNameByDbusObjPath(asyncResp, dbusObjPath, entryJsonIdx);
}

/**
 * @brief API used to fill isolated hardware details into LogEntry schema
 *        by using the given isolated dbus object.
 *
 * @param[in] asyncResp - The redfish response to return.
 * @param[in] entryJsonIdx - The json entry index to add isolated hardware
 *                            details. If passing less than or equal 0 then,
 *                            it will assume the given asyncResp jsonValue as
 *                            a single entry json object. If passing greater
 *                            than 0 then, it will assume the given asyncResp
 *                            jsonValue contains "Members" to fill in the
 *                            appropriate entry json object.
 * @param[in] dbusObjIt - The DBus object which contains isolated hardware
                         details.
 *
 * @return The redfish response with appropriate redfish properties of the
 *         isolated hardware details into LogEntry schema if success else
 *         failure response.
 */
inline void fillSystemHardwareIsolationLogEntry(
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const size_t entryJsonIdx, GetManagedObjectsType::const_iterator& dbusObjIt)
{
    nlohmann::json& entryJson =
        (entryJsonIdx > 0
             ? asyncResp->res.jsonValue["Members"][entryJsonIdx - 1]
             : asyncResp->res.jsonValue);

    for (const auto& interface : dbusObjIt->second)
    {
        if (interface.first == "xyz.openbmc_project."
                               "HardwareIsolation.Entry")
        {
            for (const auto& property : interface.second)
            {
                if (property.first == "Severity")
                {
                    const std::string* severity =
                        std::get_if<std::string>(&property.second);
                    if (severity == nullptr)
                    {
                        BMCWEB_LOG_ERROR(
                            "Failed to get the Severity from object: {}",
                            dbusObjIt->first.str);
                        messages::internalError(asyncResp->res);
                        break;
                    }

                    if (*severity == "xyz.openbmc_project."
                                     "HardwareIsolation.Entry.Type."
                                     "Critical")
                    {
                        entryJson["Severity"] = "Critical";
                    }
                    else if ((*severity == "xyz.openbmc_project."
                                           "HardwareIsolation."
                                           "Entry.Type.Warning") ||
                             (*severity == "xyz.openbmc_project."
                                           "HardwareIsolation."
                                           "Entry.Type.Manual"))
                    {
                        entryJson["Severity"] = "Warning";
                    }
                    else
                    {
                        BMCWEB_LOG_ERROR(
                            "Unsupported Severity[{}] from object: {}",
                            *severity, dbusObjIt->first.str);
                        messages::internalError(asyncResp->res);
                        break;
                    }
                }
                else if (property.first == "Resolved")
                {
                    const bool* resolved = std::get_if<bool>(&property.second);
                    if (resolved == nullptr)
                    {
                        BMCWEB_LOG_ERROR(
                            "Failed to get the Resolved from object: {}",
                            dbusObjIt->first.str);
                        messages::internalError(asyncResp->res);
                        break;
                    }
                    entryJson["Resolved"] = *resolved;
                }
            }
        }
        else if (interface.first == "xyz.openbmc_project."
                                    "Time.EpochTime")
        {
            for (const auto& property : interface.second)
            {
                if (property.first == "Elapsed")
                {
                    const uint64_t* elapsdTime =
                        std::get_if<uint64_t>(&property.second);
                    if (elapsdTime == nullptr)
                    {
                        BMCWEB_LOG_ERROR(
                            "Failed to get the Elapsed time from object: {}",
                            dbusObjIt->first.str);
                        messages::internalError(asyncResp->res);
                        break;
                    }
                    entryJson["Created"] =
                        redfish::time_utils::getDateTimeUint((*elapsdTime));
                }
            }
        }
        else if (interface.first == "xyz.openbmc_project.Association."
                                    "Definitions")
        {
            for (const auto& property : interface.second)
            {
                if (property.first == "Associations")
                {
                    const AssociationsValType* associations =
                        std::get_if<AssociationsValType>(&property.second);
                    if (associations == nullptr)
                    {
                        BMCWEB_LOG_ERROR(
                            "Failed to get the Associations from object: {}",
                            dbusObjIt->first.str);
                        messages::internalError(asyncResp->res);
                        break;
                    }
                    for (const auto& assoc : *associations)
                    {
                        if (std::get<0>(assoc) == "isolated_hw")
                        {
                            fillIsolatedHwDetailsByObjPath(
                                asyncResp,
                                sdbusplus::message::object_path(
                                    std::get<2>(assoc)),
                                entryJsonIdx);
                        }
                        else if (std::get<0>(assoc) == "isolated_hw_errorlog")
                        {
                            sdbusplus::message::object_path errPath =
                                std::get<2>(assoc);
                            entryJson["AdditionalDataURI"] =
                                "/redfish/v1/Systems/system/"
                                "LogServices/EventLog/Entries/" +
                                errPath.filename() + "/attachment";
                        }
                    }
                }
            }
        }
    }

    entryJson["@odata.type"] = "#LogEntry.v1_9_0.LogEntry";
    entryJson["@odata.id"] =
        "/redfish/v1/Systems/system/LogServices/HardwareIsolation/"
        "Entries/" +
        dbusObjIt->first.filename();
    entryJson["Id"] = dbusObjIt->first.filename();
    entryJson["Name"] = "Hardware Isolation Entry";
    entryJson["EntryType"] = "Event";
}

/**
 * @brief API Used to add the supported HardwareIsolation LogEntry Entries id
 *
 * @param[in] req - The HardwareIsolation redfish request (unused now).
 * @param[in] asyncResp - The redfish response to return.
 *
 * @return The redfish response in the given buffer.
 *
 * @note This function will return the available entries dbus object which are
 *       created by HardwareIsolation manager.
 */
inline void getSystemHardwareIsolationLogEntryCollection(
    const crow::Request& /* req */,
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp)
{
    auto getManagedObjectsHandler = [asyncResp](
                                        const boost::system::error_code ec,
                                        const GetManagedObjectsType& mgtObjs) {
        if (ec)
        {
            BMCWEB_LOG_ERROR(
                "DBUS response error [{} : {}] when tried to get the HardwareIsolation managed objects",
                ec.value(), ec.message());
            messages::internalError(asyncResp->res);
            return;
        }

        nlohmann::json& entriesArray = asyncResp->res.jsonValue["Members"];
        entriesArray = nlohmann::json::array();

        for (auto dbusObjIt = mgtObjs.begin(); dbusObjIt != mgtObjs.end();
             dbusObjIt++)
        {
            entriesArray.push_back(nlohmann::json::object());

            fillSystemHardwareIsolationLogEntry(asyncResp, entriesArray.size(),
                                                dbusObjIt);
        }

        asyncResp->res.jsonValue["Members@odata.count"] = entriesArray.size();

        asyncResp->res.jsonValue["@odata.type"] =
            "#LogEntryCollection.LogEntryCollection";
        asyncResp->res.jsonValue["@odata.id"] =
            "/redfish/v1/Systems/system/LogServices/HardwareIsolation/"
            "Entries";
        asyncResp->res.jsonValue["Name"] = "Hardware Isolation Entries";
        asyncResp->res.jsonValue["Description"] =
            "Collection of System Hardware Isolation Entries";
    };

    // Get the DBus name of HardwareIsolation service
    crow::connections::systemBus->async_method_call(
        [asyncResp, getManagedObjectsHandler](
            const boost::system::error_code ec,
            const dbus::utility::MapperGetObject& objType) {
            if (ec || objType.empty())
            {
                BMCWEB_LOG_ERROR(
                    "DBUS response error [{} : {}] when tried to get the HardwareIsolation dbus name",
                    ec.value(), ec.message());
                messages::internalError(asyncResp->res);
                return;
            }

            if (objType.size() > 1)
            {
                BMCWEB_LOG_ERROR(
                    "More than one dbus service implemented the HardwareIsolation service");
                messages::internalError(asyncResp->res);
                return;
            }

            if (objType[0].first.empty())
            {
                BMCWEB_LOG_ERROR(
                    "The retrieved HardwareIsolation dbus name is empty");
                messages::internalError(asyncResp->res);
                return;
            }

            // Fill the Redfish LogEntry schema for the retrieved
            // HardwareIsolation entries
            crow::connections::systemBus->async_method_call(
                getManagedObjectsHandler, objType[0].first,
                "/xyz/openbmc_project/hardware_isolation",
                "org.freedesktop.DBus.ObjectManager", "GetManagedObjects");
        },
        "xyz.openbmc_project.ObjectMapper",
        "/xyz/openbmc_project/object_mapper",
        "xyz.openbmc_project.ObjectMapper", "GetObject",
        "/xyz/openbmc_project/hardware_isolation",
        std::array<const char*, 1>{
            "xyz.openbmc_project.HardwareIsolation.Create"});
}

/**
 * @brief API used to route the handler for HardwareIsolation Redfish
 *        LogServices URI
 *
 * @param[in] app - Crow app on which Redfish will initialize
 *
 * @return The appropriate redfish response for the given redfish request.
 */
inline void requestRoutesSystemHardwareIsolationLogService(App& app)
{
    BMCWEB_ROUTE(app,
                 "/redfish/v1/Systems/system/LogServices/HardwareIsolation/")
        .privileges(redfish::privileges::getLogService)
        .methods(boost::beast::http::verb::get)(
            getSystemHardwareIsolationLogService);

    BMCWEB_ROUTE(
        app, "/redfish/v1/Systems/system/LogServices/HardwareIsolation/Entries/")
        .privileges(redfish::privileges::getLogEntryCollection)
        .methods(boost::beast::http::verb::get)(
            getSystemHardwareIsolationLogEntryCollection);
}

} // namespace redfish
