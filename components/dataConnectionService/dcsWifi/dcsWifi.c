//--------------------------------------------------------------------------------------------------
/**
 *  Data Channel Server's C code implementation of its southbound interfaces with the Wifi
 *  component.
 *
 *  Copyright (C) Sierra Wireless Inc.
 *
 */
//--------------------------------------------------------------------------------------------------

#include <stdlib.h>
#include <string.h>
#include "legato.h"
#include "interfaces.h"
#include "le_print.h"
#include "pa_dcs.h"
#include "dcs.h"
#include "dcsWifi.h"
#include "dcsNet.h"

#define WIFI_CONNDBS_MAX        LE_DCS_CHANNEL_LIST_QUERY_MAX  // max # of wifi conns allowed
#define WIFI_NET_INTERFACE      "wlan0"              // ToDo: as in le_data; avoid hardcoding it


//--------------------------------------------------------------------------------------------------
/**
 * The following are the defines for retry timers and their backoff parameters, including the init
 * duration & the max # of retries. After each failure to connect, the next backoff duration will
 * be doubled until the max # of retries is reached.
 *
 * There are 2 retry timers here:
 * - The connRetryTimer is for the retries over reconnecting the selected wifi connection after it
 *   failed but is still wanted by apps.
 * - The discRetryTimer is for the retries over disconnecting the selected wifi connection after
 *   it is no longer wanted while the past disconnect attempt failed.
 */
//--------------------------------------------------------------------------------------------------
#define WIFI_DISC_RETRY_MAX     3
#define WIFI_DISC_RETRY_BACKOFF_INIT 1
#define WIFI_CONN_RETRY_MAX     3
#define WIFI_CONN_RETRY_BACKOFF_INIT 1


//--------------------------------------------------------------------------------------------------
/**
 * The following are DCS's global data structures for storing wifi scan info and connection info.
 * Unlike cellular, wifi can only have 1 active connection at a time, although its list of available
 * connections/SSIDs recorded on the activeList and in WiFiConnDbPoolwould have many.
 */
//--------------------------------------------------------------------------------------------------
typedef struct
{
    bool isActive;                                  ///< whether scanning is active in action
    uint16_t listSize;                              ///< size of activeList below being filled
    le_dcs_ChannelInfo_t activeList[LE_DCS_CHANNEL_LIST_QUERY_MAX];  ///< list of active SSIDs
} DcsWifiScan_t;
static DcsWifiScan_t DcsWifiScan;

typedef struct
{
    le_wifiClient_NewEventHandlerRef_t eventHandlerRef;
    le_dls_List_t dbList;                            ///< list of active wifiConnDbs
    wifi_connDb_t *selectedConnDb;                   ///< connDb of the selected connection
    le_wifiClient_AccessPointRef_t apRef;            ///< AP reference of the selected connection
    bool hasStarted;                                 ///< if DCS has called le_wifiClient_Start()
    bool opStateUp;                                  ///< operational state
    wifi_connDb_t *disconnectingConn;                ///< Connect with disconnect initiated
    le_timer_Ref_t discRetryTimer;                   ///< disconnect retry timer
    uint16_t discRetries;                            ///< # of disconnect retries attempted
    uint16_t discRetryBackoff;                       ///< latest backoff length for next disc retry
    le_timer_Ref_t connRetryTimer;                   ///< connection retry timer
    uint16_t connRetries;                            ///< # of connect retries attempted
    uint16_t connRetryBackoff;                       ///< latest backoff length for next conn retry
} DcsWifi_t;
static DcsWifi_t DcsWifi;

static le_mem_PoolRef_t WifiConnDbPool;


//--------------------------------------------------------------------------------------------------
/**
 * Safe Reference Map for wifi connection database objects
 */
//--------------------------------------------------------------------------------------------------
static le_ref_MapRef_t WifiConnectionRefMap;


//--------------------------------------------------------------------------------------------------
/**
 * Search for the given Wifi connection reference's connDb from its reference map
 *
 * @return
 *     - The found connDb will be returned in the function's return value; otherwise, NULL
 */
//--------------------------------------------------------------------------------------------------
static wifi_connDb_t *DcsWifiGetDbFromRef
(
    le_dcs_WifiConnectionRef_t wifiConnRef
)
{
    if (!wifiConnRef)
    {
        return NULL;
    }
    return (wifi_connDb_t *)le_ref_Lookup(WifiConnectionRefMap, wifiConnRef);
}


//--------------------------------------------------------------------------------------------------
/**
 * Search for the wifi connection db of the selected SSID to be connected
 *
 * @return
 *     - The found wifi connection db; otherwise NULL
 */
//--------------------------------------------------------------------------------------------------
static wifi_connDb_t *DcsWifiGetSelectedDb
(
    void
)
{
    le_result_t ret;
    uint8_t ssid[LE_WIFIDEFS_MAX_SSID_BYTES];
    size_t ssidSize = LE_WIFIDEFS_MAX_SSID_BYTES;
    le_wifiClient_AccessPointRef_t apRef = NULL;
    le_dcs_channelDb_t *channelDb;

    if (DcsWifi.selectedConnDb)
    {
        return DcsWifi.selectedConnDb;
    }

    // For the case in which the wifi connection was brought up via le_data so that le_dcs hasn't
    // recorded it into DcsWifi.selectedConnDb, query le_wifiClient for the currently selected
    // connection
    le_wifiClient_GetCurrentConnection(&apRef);
    if (NULL == apRef)
    {
        return NULL;
    }
    ret = le_wifiClient_GetSsid(apRef, &ssid[0], &ssidSize);
    if (LE_OK != ret)
    {
        LE_ERROR("Failed to find SSID of AP reference %p", apRef);
        return NULL;
    }
    ssid[ssidSize] = '\0';
    LE_DEBUG("Found currently selected Wifi connection to get established: %s, reference %p",
             ssid, apRef);
    channelDb = le_dcs_GetChannelDbFromName((const char *)ssid, LE_DCS_TECH_WIFI);
    if (!channelDb)
    {
        LE_ERROR("Failed to find channel db for SSID %s", ssid);
        return NULL;
    }
    return DcsWifiGetDbFromRef((le_dcs_WifiConnectionRef_t)channelDb->techRef);
}


//--------------------------------------------------------------------------------------------------
/**
 * Function to check upfront if the Wifi technology allows channel start on the given connection.
 * The present restriction imposed here is only 1 single active wifi connection.
 *
 * @return
 *     - LE_OK if there's no active wifi connection yet
 *     - LE_DUPLICATE if there's already an active connection which is the given one in the input
 *     - LE_NOT_PERMITTED if there's already an active connection which is not the given one
 */
//--------------------------------------------------------------------------------------------------
le_result_t le_dcsWifi_AllowChannelStart
(
    void *techRef
)
{
    le_dcs_WifiConnectionRef_t wifiConnRef = (le_dcs_WifiConnectionRef_t)techRef;
    wifi_connDb_t *selectedConnDb = DcsWifiGetSelectedDb();
    if (!selectedConnDb)
    {
        return LE_OK;
    }

    if (selectedConnDb->connRef == wifiConnRef)
    {
        return LE_DUPLICATE;
    }

    if (DcsWifiScan.isActive)
    {
        LE_ERROR("Starting a wifi connection disallowed while wifi scanning is in progress");
    }

    return LE_NOT_PERMITTED;
}


//--------------------------------------------------------------------------------------------------
/**
 * This function checks if DCS has already started wifiClient.  If not, call to start it.
 *
 * @return
 *     - LE_OK upon success in starting wifiClient anew
 *     - LE_DUPLICATE if DCS has it started already
 *     - LE_BUSY if wifiClient has already been started by its other client
 */
//--------------------------------------------------------------------------------------------------
static le_result_t DcsWifiClientStart
(
    void
)
{
    le_result_t ret;

    if (DcsWifi.hasStarted)
    {
        LE_DEBUG("DCS already got wifiClient started");
        return LE_DUPLICATE;
    }

    LE_INFO("Starting Wifi client");
    ret = le_wifiClient_Start();
    if ((LE_OK == ret) || (LE_BUSY == ret))
    {
        LE_DEBUG("DCS got wifiClient started; return code %d", ret);
        DcsWifi.hasStarted = true;
    }
    return ret;
}


//--------------------------------------------------------------------------------------------------
/**
 * This function seeks to stop wifiClient if not yet by le_dcs.
 */
//--------------------------------------------------------------------------------------------------
static void DcsWifiClientStop
(
    void
)
{
    le_result_t ret;

    if (!DcsWifi.hasStarted)
    {
        LE_DEBUG("DCS hasn't started wifiClient and thus won't seek to stop it");
        return;
    }

    if (le_timer_IsRunning(DcsWifi.discRetryTimer))
    {
        // To be safe, stop the disconnect retry timer if it's running
        le_timer_Stop(DcsWifi.discRetryTimer);
    }

    if (DcsWifiScan.isActive)
    {
        LE_DEBUG("Delay wifiClient stop when wifi scanning is in progress");
        return;
    }

    LE_INFO("Stopping Wifi client");
    ret = le_wifiClient_Stop();
    if ((ret != LE_OK) && (ret != LE_DUPLICATE))
    {
        LE_ERROR("DCS failed to stop wifiClient");
    }
    else
    {
        LE_INFO("DCS succeeded to stop wifiClient");
        DcsWifi.hasStarted = false;
    }
}


//--------------------------------------------------------------------------------------------------
/**
 * This function performs the necessary clean up to reset le_wifiClient and DcsWifi so that it is
 * clean enough for the next Connect Request to pass
 */
//--------------------------------------------------------------------------------------------------
static void DcsWifiPostFailureReset
(
    void
)
{
    DcsWifi.apRef = NULL;
    DcsWifi.selectedConnDb = NULL;

    // Still call le_wifiClient_Disconnect() to reset authentication upon. Otherwise, next connect
    // attempt will get authentication failure
    le_wifiClient_Disconnect();

    DcsWifiClientStop();
}


//--------------------------------------------------------------------------------------------------
/**
 * This function seeks to disconnect wifiClient. A prerequisite for calling this function is to have
 * DcsWifi.selectedConnDb checked in the caller to make sure it's not NULL.
 *
 * @return
 *     - LE_OK upon success to newly initiate a disconnect
 *     - LE_DUPLICATE upon a duplicated attempt to disconnect
 *     - LE_FAULT upon failure to initiate a disconnect
 */
//--------------------------------------------------------------------------------------------------
static le_result_t DcsWifiClientDisconnect
(
    wifi_connDb_t *wifiConnDb
)
{
    le_clk_Time_t retryInterval = {DcsWifi.discRetryBackoff, 0};
    DcsWifi.disconnectingConn = wifiConnDb;
    le_result_t ret = le_wifiClient_Disconnect();
    if ((ret == LE_OK) || (ret == LE_DUPLICATE))
    {
        LE_INFO("Succeeded triggering wifi disconnect over SSID %s", wifiConnDb->ssid);
        return ret;
    }

    LE_DEBUG("Failed to immediately disconnect over SSID %s; error: %d", wifiConnDb->ssid, ret);

    DcsWifi.disconnectingConn = NULL;
    if (le_timer_IsRunning(DcsWifi.discRetryTimer))
    {
        // Disconnect retry timer is already running
        return LE_DUPLICATE;
    }

    if (DcsWifi.discRetries >= WIFI_DISC_RETRY_MAX)
    {
        LE_INFO("Disconnect retries exhausted over SSID %s", wifiConnDb->ssid);
        return LE_FAULT;
    }

    if (LE_OK != le_timer_SetContextPtr(DcsWifi.discRetryTimer, wifiConnDb->connRef))
    {
        LE_ERROR("Failed to set timer context to attempt disconnect retry over SSID %s",
                 wifiConnDb->ssid);
        return LE_FAULT;
    }

    if ((LE_OK != le_timer_SetInterval(DcsWifi.discRetryTimer, retryInterval)) ||
        (LE_OK != le_timer_Start(DcsWifi.discRetryTimer)))
    {
        LE_ERROR("Failed to start disconnect retry timer for SSID %s", wifiConnDb->ssid);
        return LE_FAULT;
    }

    LE_DEBUG("Attempt #%d started to disconnect connection over SSID %s", DcsWifi.discRetries,
             wifiConnDb->ssid);

    LE_INFO("Initiated disconnect retry timer for %s; attempt %d, backoff %d secs",
            wifiConnDb->ssid, DcsWifi.discRetries, DcsWifi.discRetryBackoff);
    DcsWifi.discRetries++;
    DcsWifi.discRetryBackoff = DcsWifi.discRetryBackoff * 2;
    return LE_OK;
}


//--------------------------------------------------------------------------------------------------
/**
 * Search for a wifiConnDb from its SSID
 *
 * @return
 *     - The found channelDb will be returned in the function's return value; otherwise, NULL
 */
//--------------------------------------------------------------------------------------------------
static wifi_connDb_t *DcsWifiGetDbFromSsid
(
    const char *ssid
)
{
    le_ref_IterRef_t iterRef = le_ref_GetIterator(WifiConnectionRefMap);
    wifi_connDb_t *wifiConnDb;

    if (!ssid)
    {
        return NULL;
    }

    while (le_ref_NextNode(iterRef) == LE_OK)
    {
        wifiConnDb = (wifi_connDb_t *)le_ref_GetValue(iterRef);
        if (strncmp(wifiConnDb->ssid, ssid, LE_DCS_CHANNEL_NAME_MAX_LEN) == 0)
        {
            return wifiConnDb;
        }
    }
    return NULL;
}


//--------------------------------------------------------------------------------------------------
/**
 * This function retrieves the network interface of the given Wifi connection
 *
 * @return
 *     - the name of the net interface as a character string in the presence of a connected Wifi
 *       connection; otherwise a null string
 */
//--------------------------------------------------------------------------------------------------
static char *DcsWifiGetNetInterface
(
    wifi_connDb_t *wifiConnDb
)
{
    wifi_connDb_t *selectedConnDb = DcsWifiGetSelectedDb();
    if (!selectedConnDb)
    {
        return "";
    }

    return ((wifiConnDb == selectedConnDb) ? WIFI_NET_INTERFACE : "");
}


//--------------------------------------------------------------------------------------------------
/**
 * Function for querying the network interface of the given connection specified in the 1st argument
 *
 * @return
 *     - The retrieved network interface's name will be returned in the 2nd argument which allowed
 *       buffer length is specified in the 3rd argument that is to be observed strictly
 *     - The function returns LE_OK upon a successful retrieval; otherwise, some other le_result_t
 *       failure cause
 */
//--------------------------------------------------------------------------------------------------
le_result_t le_dcsWifi_GetNetInterface
(
    void *techRef,
    char *intfName,
    int nameSize
)
{
    char *netInterface;
    wifi_connDb_t *wifiConnDb, *selectedConnDb;
    le_dcs_WifiConnectionRef_t wifiConnRef = (le_dcs_WifiConnectionRef_t)techRef;

    intfName[0] = '\0';
    wifiConnDb = DcsWifiGetDbFromRef(wifiConnRef);
    if (!wifiConnDb)
    {
        LE_DEBUG("Failed to get network interface of connection with reference %p already gone",
                 wifiConnRef);
        return LE_FAULT;
    }

    selectedConnDb = DcsWifiGetSelectedDb();
    if (wifiConnDb != selectedConnDb)
    {
        LE_DEBUG("Failed to get network interface of a down SSID %s", wifiConnDb->ssid);
        return LE_FAULT;
    }

    netInterface = DcsWifiGetNetInterface(wifiConnDb);
    if (!netInterface || (strlen(netInterface) > nameSize))
    {
        LE_ERROR("Failed to get network interface of SSID %s", wifiConnDb->ssid);
        return LE_FAULT;
    }

    strncpy(intfName, netInterface, nameSize);
    LE_DEBUG("SSID %s is connected over network interface %s", wifiConnDb->ssid, intfName);
    return LE_OK;
}


//--------------------------------------------------------------------------------------------------
/**
 * Function for retrieving the network interface state of the given wifi connection
 *
 * @return
 *     - true if up; false otherwise
 */
//--------------------------------------------------------------------------------------------------
static bool DcsWifiGetNetInterfaceStateUp
(
    wifi_connDb_t *wifiConnDb
)
{
    bool state;
    char *netInterface;

    if (!wifiConnDb)
    {
        LE_ERROR("Invalid wifi connection db for retrieving interface state");
        return false;
    }

    netInterface = DcsWifiGetNetInterface(wifiConnDb);
    if (!netInterface || (LE_OK != le_net_GetNetIntfState(netInterface, &state)))
    {
        LE_ERROR("Failed to get network interface state of SSID %s", wifiConnDb->ssid);
        return false;
    }

    return state;
}


//--------------------------------------------------------------------------------------------------
/**
 * Function for checking if the given wifi connection db's operational state is up or not
 *
 * @return
 *     - In the function's return value, the bool is returned to indicate whether the given
 *       connection's techRef is up or not
 */
//--------------------------------------------------------------------------------------------------
bool le_dcsWifi_GetOpState
(
    void *techRef
)
{
    wifi_connDb_t *wifiConnDb, *selectedConnDb;
    le_dcs_WifiConnectionRef_t wifiConnRef = (le_dcs_WifiConnectionRef_t)techRef;

    selectedConnDb = DcsWifiGetSelectedDb();
    if (!selectedConnDb)
    {
        LE_DEBUG("Wifi state is down before it's brought up");
        return false;
    }

    if (wifiConnRef != selectedConnDb->connRef)
    {
        wifiConnDb = DcsWifiGetDbFromRef(wifiConnRef);
        LE_DEBUG("Wifi has connection over SSID %s up rather than SSID %s",
                 selectedConnDb->ssid, wifiConnDb ? wifiConnDb->ssid : "");
        return false;
    }

    if (DcsWifi.selectedConnDb)
    {
        return DcsWifi.opStateUp;
    }
    else
    {
        // This wifi connection was started via le_data
        return DcsWifiGetNetInterfaceStateUp(selectedConnDb);
    }
}


//--------------------------------------------------------------------------------------------------
/**
 * Function for internally creating a connection db of the wifi type for the given SSID
 *
 * @return
 *     - The newly created wifi connection db is returned in this function's return value upon
 *       successful creation; otherwise NULL
 */
//--------------------------------------------------------------------------------------------------
static wifi_connDb_t *DcsWifiCreateConnDb
(
    const char *ssid
)

{
    wifi_connDb_t *wifiConnDb;

    if (!ssid || (strlen(ssid) == 0))
    {
        LE_ERROR("Cannot create wifi connection db with null SSID");
        return NULL;
    }

    wifiConnDb = DcsWifiGetDbFromSsid(ssid);
    if (wifiConnDb)
    {
        return wifiConnDb;
    }

    wifiConnDb = le_mem_ForceAlloc(WifiConnDbPool);
    if (!wifiConnDb)
    {
        LE_ERROR("Unable to alloc wifi Db for SSID %s", ssid);
        return NULL;
    }

    memset(wifiConnDb, 0, sizeof(wifi_connDb_t));
    strncpy(wifiConnDb->ssid, ssid, strlen(ssid));
    wifiConnDb->connRef = le_ref_CreateRef(WifiConnectionRefMap, wifiConnDb);
    wifiConnDb->dbLink = LE_DLS_LINK_INIT;
    le_dls_Queue(&DcsWifi.dbList, &(wifiConnDb->dbLink));

    LE_DEBUG("ConnRef %p created for wifi SSID %s", wifiConnDb->connRef, ssid);
    return (wifiConnDb);
}


//--------------------------------------------------------------------------------------------------
/**
 * Function for creating a wifi connection db of the given connection if it's not present yet.
 * If present, it will set itself into the given connection's connDb
 *
 * @return
 *     - The object reference to the newly created wifi connection db is returned in this
 *       function's return value upon successful creation or found existence; otherwise NULL
 */
//--------------------------------------------------------------------------------------------------
void *le_dcsWifi_CreateConnDb
(
    const char *conn
)
{
    wifi_connDb_t *wifiConnDb = DcsWifiGetDbFromSsid(conn);
    if (!wifiConnDb && !(wifiConnDb = DcsWifiCreateConnDb(conn)))
    {
        LE_ERROR("Failed to create wifi connection db for SSID %s", conn);
        return NULL;
    }

    return ((void *)wifiConnDb->connRef);
}


//--------------------------------------------------------------------------------------------------
/**
 * Destructor function that runs when a wifi connection db is deallocated
 */
//--------------------------------------------------------------------------------------------------
static void DcsWifiConnDbDestructor
(
    void *objPtr
)
{
    wifi_connDb_t *wifiConnDb = (wifi_connDb_t *)objPtr;
    if (!wifiConnDb)
    {
        return;
    }

    le_dls_Remove(&DcsWifi.dbList, &(wifiConnDb->dbLink));
    wifiConnDb->dbLink = LE_DLS_LINK_INIT;
    le_ref_DeleteRef(WifiConnectionRefMap, wifiConnDb->connRef);
    wifiConnDb->connRef = NULL;
}


//--------------------------------------------------------------------------------------------------
/**
 * Function for releasing a wifi_connDb_t back to free memory after it's looked up from the
 * given reference in the argument
 */
//--------------------------------------------------------------------------------------------------
void le_dcsWifi_ReleaseConnDb
(
    void *techRef
)
{
    le_dcs_WifiConnectionRef_t wifiConnRef = (le_dcs_WifiConnectionRef_t)techRef;
    wifi_connDb_t *wifiConnDb = DcsWifiGetDbFromRef(wifiConnRef);
    if (wifiConnDb)
    {
        le_mem_Release(wifiConnDb);
    }
}


//--------------------------------------------------------------------------------------------------
/**
 * Function for traversing DcsWifiScan.activeList[] and create for each SSID there a wifiConnDb
 */
//--------------------------------------------------------------------------------------------------
static void DcsWifiUpdateConnDbList
(
    void
)
{
    uint16_t i;

    for (i=0 ; i<DcsWifiScan.listSize; i++)
    {
        (void)DcsWifiCreateConnDb(DcsWifiScan.activeList[i].name);
    }
}


//--------------------------------------------------------------------------------------------------
/**
 * Function for checking if the given SSID in the argument is on the active SSID list
 * DcsWifiScan.activeList[] obtained via the last wifi scan.
 *
 * @return
 *     - true if the SSID is on the active list; false otherwise
 */
//--------------------------------------------------------------------------------------------------
static bool DcsWifiSsidOnActiveList
(
    const char *ssid
)
{
    uint16_t i;

    if (!ssid || (strlen(ssid) == 0))
    {
        return false;
    }

    for (i=0; i<DcsWifiScan.listSize; i++)
    {
        if (strncmp(ssid, DcsWifiScan.activeList[i].name, LE_DCS_CHANNEL_NAME_MAX_LEN) == 0)
        {
            return true;
        }
    }

    return false;
}


//--------------------------------------------------------------------------------------------------
/**
 * Function for traversing the list of all wifiConnDbs on DcsWifi.dbList and check each SSID there to
 * see if it is still on the active SSID list DcsWifiScan.activeList[].  If not, remove its
 * wifiConnDb.
 *
 */
//--------------------------------------------------------------------------------------------------
static void DcsWifiPurgeConnDbs
(
    void
)
{
    wifi_connDb_t *wifiConnDb, *selectedConnDb = DcsWifiGetSelectedDb();
    le_dls_Link_t* linkPtr = le_dls_Peek(&DcsWifi.dbList);

    while (NULL != linkPtr)
    {
        wifiConnDb = CONTAINER_OF(linkPtr, wifi_connDb_t, dbLink);
        linkPtr = le_dls_PeekNext(&DcsWifi.dbList, linkPtr);

        if (!wifiConnDb || (strlen(wifiConnDb->ssid) == 0))
        {
            continue;
        }
        if (selectedConnDb &&
            (strncmp(wifiConnDb->ssid, selectedConnDb->ssid, LE_DCS_CHANNEL_NAME_MAX_LEN) == 0))
        {
            // Do not purge the selected wifiConnDb in action
            continue;
        }
        if (DcsWifiSsidOnActiveList(wifiConnDb->ssid))
        {
            // Do not purge the wifiConnDb which is still on the active wifi SSID list
            continue;
        }

        // Purge the wifiConnDb which is no longer on the active wifi SSID list, via purging
        // its DCS channel which has to run & finish synchronously
        LE_DEBUG("Purging unavailable & unused Wifi channel with ssid %s", wifiConnDb->ssid);
        if (!le_dcs_DeleteChannelDb(LE_DCS_TECH_WIFI, wifiConnDb->connRef))
        {
            LE_ERROR("Failed to purge unavailable & unused Wifi channel with ssid %s",
                     wifiConnDb->ssid);
        }
    }
}


//--------------------------------------------------------------------------------------------------
/**
 * Reads the wifi scan results from le_wifiClient
 */
//--------------------------------------------------------------------------------------------------
static void WifiReadScanResults
(
    void
)
{
    le_wifiClient_AccessPointRef_t apRef = 0;
    wifi_connDb_t *wifiConnDb, *selectedConnDb;

    LE_DEBUG("Get Wifi scan results");
    DcsWifiScan.isActive = false;

    apRef = le_wifiClient_GetFirstAccessPoint();
    if (!apRef)
    {
        LE_ERROR("le_wifiClient_GetFirstAccessPoint ERROR");
        le_dcsTech_CollectChannelQueryResults(LE_DCS_TECH_WIFI, LE_FAULT, DcsWifiScan.activeList,
                                              0);
        if (!DcsWifi.selectedConnDb)
        {
            DcsWifiClientStop();
        }
        return;
    }

    // Rebuild the DcsWifiScan.activeList
    memset(&DcsWifiScan.activeList, 0x0, sizeof(DcsWifiScan.activeList));
    DcsWifiScan.listSize = 0;
    selectedConnDb = DcsWifiGetSelectedDb();

    do
    {
        uint8_t ssid[LE_WIFIDEFS_MAX_SSID_BYTES];
        size_t ssidSize = LE_WIFIDEFS_MAX_SSID_BYTES;
        char bssid[LE_WIFIDEFS_MAX_BSSID_BYTES] = {0};
        size_t bssidSize = LE_WIFIDEFS_MAX_BSSID_BYTES;
        int16_t signalStrength;
        le_result_t ret;
        bool state;

        ret = le_wifiClient_GetSsid(apRef, &ssid[0], &ssidSize);
        if (LE_OK != ret)
        {
            LE_ERROR("Wifi result: SSID = unknown (error %d)", ret);
            apRef = le_wifiClient_GetNextAccessPoint();
            continue;
        }
        if (ssidSize > (LE_DCS_CHANNEL_NAME_MAX_LEN-1))
        {
            // Skip SSID which length is longer than what DCS can take
            apRef = le_wifiClient_GetNextAccessPoint();
            continue;
        }
        ssid[ssidSize] = '\0';
        if (DcsWifiSsidOnActiveList((const char *)ssid))
        {
            apRef = le_wifiClient_GetNextAccessPoint();
            continue;
        }

        signalStrength = le_wifiClient_GetSignalStrength(apRef);
        (void)le_wifiClient_GetBssid(apRef, bssid, bssidSize);
        LE_INFO("Wifi result: SSID = %.*s, BSSID = %.*s, AP reference %p, signal strength = %d",
                (int)ssidSize, (char *)&ssid[0], (int)bssidSize, &bssid[0], apRef, signalStrength);

        strncpy(DcsWifiScan.activeList[DcsWifiScan.listSize].name, (char *)ssid, ssidSize);
        DcsWifiScan.activeList[DcsWifiScan.listSize].name[ssidSize] = '\0';
        DcsWifiScan.activeList[DcsWifiScan.listSize].technology = LE_DCS_TECH_WIFI;
        if (selectedConnDb && (strlen(selectedConnDb->ssid) == ssidSize) &&
            (strncmp((char *)ssid, selectedConnDb->ssid, ssidSize) == 0))
        {
            // Since this is the selected SSID for connecting, return its present state
            if (DcsWifi.selectedConnDb)
            {
                state = DcsWifi.opStateUp;
            }
            else
            {
                // This wifi connection was started via le_data
                state = DcsWifiGetNetInterfaceStateUp(selectedConnDb);
            }
            DcsWifiScan.activeList[DcsWifiScan.listSize].state = state ?
                LE_DCS_STATE_UP : LE_DCS_STATE_DOWN;
        }
        else
        {
            DcsWifiScan.activeList[DcsWifiScan.listSize].state = LE_DCS_STATE_DOWN;
        }

        wifiConnDb = DcsWifiGetDbFromSsid((const char *)ssid);
        if (wifiConnDb)
        {
            LE_DEBUG("This is an already known SSID");
        }
        else
        {
            // Create wifiConnDb for newly learned SSID
            LE_DEBUG("This is a newly learned SSID");
            wifiConnDb = DcsWifiCreateConnDb((const char *)ssid);
        }

        if (wifiConnDb)
        {
            DcsWifiScan.activeList[DcsWifiScan.listSize].ref = wifiConnDb->connRef;
            DcsWifiScan.listSize++;
        }
        apRef = le_wifiClient_GetNextAccessPoint();
    } while (apRef && (DcsWifiScan.listSize < LE_DCS_CHANNEL_LIST_QUERY_MAX));

    LE_INFO("Wifi SSID results: %d found", DcsWifiScan.listSize);

    if (DcsWifiScan.listSize)
    {
        DcsWifiPurgeConnDbs();
        DcsWifiUpdateConnDbList();
    }
    le_dcsTech_CollectChannelQueryResults(LE_DCS_TECH_WIFI, LE_OK, DcsWifiScan.activeList,
                                          DcsWifiScan.listSize);
    if (!DcsWifi.selectedConnDb)
    {
        DcsWifiClientStop();
    }
}


//--------------------------------------------------------------------------------------------------
/**
 * Function to get the list of all available wifi channels, i.e. SSIDs
 *
 * @return
 *     - The function returns LE_OK upon a successful trigger; otherwise, some other
 *       le_result_t failure cause
 */
//--------------------------------------------------------------------------------------------------
le_result_t le_dcsWifi_GetChannelList
(
    void
)
{
    le_result_t ret;

    if (DcsWifiScan.isActive)
    {
        LE_DEBUG("Wifi scan already in progress");
        return LE_DUPLICATE;
    }

    ret = DcsWifiClientStart();
    if ((LE_OK != ret) && (LE_BUSY != ret) && (LE_DUPLICATE != ret))
    {
        LE_ERROR("Failed to start wifiClient up for scanning");
        le_dcsTech_CollectChannelQueryResults(LE_DCS_TECH_WIFI, LE_FAULT, NULL, 0);
        DcsWifiClientStop();
        return ret;
    }

    ret = le_wifiClient_Scan();
    DcsWifiScan.isActive = true;
    if (ret != LE_OK)
    {
        LE_ERROR("Failed to initiate a Wifi channel scan; error: %d", ret);
        DcsWifiScan.isActive = false;
    }
    return ret;
}


//--------------------------------------------------------------------------------------------------
/**
 * This function processes the Connected event from wifiClient
 */
//--------------------------------------------------------------------------------------------------
static void DcsWifiClientConnected
(
    void
)
{
    le_result_t ret;
    char *netInterface;
    le_dcs_ChannelRef_t channelRef;
    wifi_connDb_t *selectedConnDb = DcsWifiGetSelectedDb();
    if (!selectedConnDb)
    {
        LE_DEBUG("Ignore irrelevant wifi Connected event");
        return;
    }

    if (le_timer_IsRunning(DcsWifi.connRetryTimer))
    {
        le_timer_Stop(DcsWifi.connRetryTimer);
    }

    if (!DcsWifi.selectedConnDb)
    {
        // This wifi connection was started via le_data
        LE_INFO("Wifi client connected on SSID %s", selectedConnDb->ssid);
        channelRef = le_dcs_GetChannelRefFromTechRef(LE_DCS_TECH_WIFI, selectedConnDb->connRef);
        if (channelRef)
        {
            le_dcs_ChannelEventNotifier(channelRef, LE_DCS_EVENT_UP);
        }
        return;
    }

    LE_INFO("Wifi client connected on SSID %s", DcsWifi.selectedConnDb->ssid);

    channelRef = le_dcs_GetChannelRefFromTechRef(LE_DCS_TECH_WIFI,
                                                 DcsWifi.selectedConnDb->connRef);
    if (!channelRef)
    {
        LE_ERROR("Failed to get channel db reference to send wifi connection up event");
        ret = DcsWifiClientDisconnect(DcsWifi.selectedConnDb);
        if ((ret != LE_OK) && (ret != LE_DUPLICATE))
        {
            DcsWifiPostFailureReset();
        }
        // Wait till the Disconnected event from wifiClient before sending the Down event
        // notification
        return;
    }

    // Set the state Up before processing with other subsequent function calls
    DcsWifi.opStateUp = true;
    netInterface = DcsWifiGetNetInterface(DcsWifi.selectedConnDb);
    if (!netInterface || (strlen(netInterface) == 0) ||
        (LE_OK != pa_dcs_AskForIpAddress(netInterface)))
    {
        LE_ERROR("Failed to bring up IP on wifi connection over SSID %s",
                 DcsWifi.selectedConnDb->ssid);
        DcsWifi.opStateUp = false;
        ret = DcsWifiClientDisconnect(DcsWifi.selectedConnDb);
        if ((ret != LE_OK) && (ret != LE_DUPLICATE))
        {
            // Send the down event notification here as the Disconnect Request to wifiClient failed
            le_dcs_ChannelEventNotifier(channelRef, LE_DCS_EVENT_DOWN);
            DcsWifiPostFailureReset();
        }
        // Wait till the Disconnected event from wifiClient before sending the Down event
        // notification
        return;
    }

    LE_INFO("Wifi connection over SSID %s on net interface %s got IP up",
            DcsWifi.selectedConnDb->ssid, netInterface);

    // Send connection up event to apps
    le_dcs_ChannelEventNotifier(channelRef, LE_DCS_EVENT_UP);
}


//--------------------------------------------------------------------------------------------------
/**
 * Function to initiate a connection retry by starting the connection's retry timer upon which
 * expiry the retry will be carried out
 *
 * @return
 *     - It returns true if an upcoming retry will happen no matter who has initiated it; false
 *       otherwise
 */
//--------------------------------------------------------------------------------------------------
static le_result_t DcsWifiRetryConn
(
    wifi_connDb_t *wifiConnDb
)
{
    le_clk_Time_t retryInterval = {DcsWifi.connRetryBackoff, 0};

    if (!wifiConnDb)
    {
        // Not supposed, but for safety
        LE_ERROR("Failed to initiate connection retry with the connection db already gone");
        return LE_FAULT;
    }

    if (le_timer_IsRunning(DcsWifi.connRetryTimer))
    {
        LE_DEBUG("Wifi connection be retried after next retry timer expiry");
        return LE_DUPLICATE;
    }

    if (DcsWifi.connRetries > WIFI_CONN_RETRY_MAX)
    {
        LE_INFO("Wifi connection %s already maxed out retry allowed (%d)", wifiConnDb->ssid,
                WIFI_CONN_RETRY_MAX);
        return LE_OVERFLOW;
    }

    // set connection retry timer's context & duration & then start it
    if (LE_OK != le_timer_SetContextPtr(DcsWifi.connRetryTimer, wifiConnDb->connRef))
    {
        LE_ERROR("Failed to set timer context to attempt connection retry for %s",
                 wifiConnDb->ssid);
        return LE_FAULT;
    }

    if ((LE_OK != le_timer_SetInterval(DcsWifi.connRetryTimer, retryInterval)) ||
        (LE_OK != le_timer_Start(DcsWifi.connRetryTimer)))
    {
        LE_ERROR("Failed to start connect retry timer for SSID %s", wifiConnDb->ssid);
        return LE_FAULT;
    }

    // update retry count & backoff duration for next round
    LE_INFO("Initiated connect retry timer for %s; attempt %d, backoff %d secs",
            wifiConnDb->ssid, DcsWifi.connRetries, DcsWifi.connRetryBackoff);
    DcsWifi.connRetries++;
    DcsWifi.connRetryBackoff = DcsWifi.connRetryBackoff * 2;

    // Need to call le_wifiClient_Disconnect to reset it as prep for the next retry
    le_wifiClient_Disconnect();

    return LE_OK;
}


//--------------------------------------------------------------------------------------------------
/**
 * This function processes the Disconnected event from wifiClient
 */
//--------------------------------------------------------------------------------------------------
static void DcsWifiClientDisconnected
(
    void
)
{
    uint16_t refcount;
    le_result_t ret;
    char *ssid;
    le_dcs_ChannelRef_t channelRef;
    wifi_connDb_t *selectedConnDb = DcsWifiGetSelectedDb();
    if (!DcsWifi.disconnectingConn)
    {
        LE_DEBUG("Handle passive wifi Disconnected event");

        // It's safe to anyway set DcsWifi.selectedConnDb to NULL because a Disconnect has
        // completed, and it's possible that le_dcs has given up this connection to le_data
        // and let it perform the actual disconnect, in which case when this Disconnected event
        // arrives here, DcsWifi.disconnectingConn isn't true, but DcsWifi.selectedConnDb has
        // to be reset
        DcsWifi.selectedConnDb = NULL;
        DcsWifi.apRef = NULL;
        DcsWifiClientStop();
        return;
    }

    selectedConnDb = DcsWifi.disconnectingConn;
    DcsWifi.disconnectingConn = NULL;

    if (!DcsWifi.selectedConnDb)
    {
        // This wifi connection was started via le_data. Thus, do no more than sending a
        // notification
        LE_INFO("Wifi client disconnected on SSID %s", selectedConnDb->ssid);
        channelRef = le_dcs_GetChannelRefFromTechRef(LE_DCS_TECH_WIFI, selectedConnDb->connRef);
        if (channelRef)
        {
            le_dcs_ChannelEventNotifier(channelRef, LE_DCS_EVENT_DOWN);
        }
        return;
    }

    ssid = DcsWifi.selectedConnDb->ssid;
    LE_INFO("Wifi client disconnected over SSID %s", ssid);
    DcsWifi.opStateUp = false;

    if (le_dcs_GetChannelRefCountFromTechRef(LE_DCS_TECH_WIFI, DcsWifi.selectedConnDb->connRef,
                                             &refcount) == LE_OK)
    {
        if (refcount > 0)
        {
            // Need to retry connecting until exhausted
            ret = DcsWifiRetryConn(DcsWifi.selectedConnDb);
            switch (ret)
            {
                case LE_OK:
                    LE_INFO("Wait for the next retry before failing connection %s", ssid);
                    return;
                case LE_DUPLICATE:
                    LE_DEBUG("No need to re-trigger retry for connection %s", ssid);
                    return;
                case LE_OVERFLOW:
                case LE_FAULT:
                default:
                    break;
            }
        }
    }
    else
    {
        LE_ERROR("Failed to get refcount of wifi connection %s to handle Disconnected event",
                 ssid);
    }

    // Send connection down event to apps
    LE_DEBUG("Send Down event of connection %s to apps", ssid);
    channelRef = le_dcs_GetChannelRefFromTechRef(LE_DCS_TECH_WIFI,
                                                 DcsWifi.selectedConnDb->connRef);
    if (!channelRef)
    {
        LE_ERROR("Failed to get channel db reference to send wifi connection down event");
    }
    else
    {
        le_dcs_ChannelEventNotifier(channelRef, LE_DCS_EVENT_DOWN);
    }
    DcsWifi.selectedConnDb = NULL;
    DcsWifi.apRef = NULL;
    DcsWifiClientStop();
}


//--------------------------------------------------------------------------------------------------
/**
 * Event callback for Wifi Client events
 */
//--------------------------------------------------------------------------------------------------
static void WifiClientEventHandler
(
    le_wifiClient_Event_t event,    ///< [IN] Wifi event
    void* contextPtr                ///< [IN] Associated context pointer
)
{
    LE_DEBUG("Wifi event received %d", event);

    switch (event)
    {
        case LE_WIFICLIENT_EVENT_CONNECTED:
            DcsWifiClientConnected();
            break;

        case LE_WIFICLIENT_EVENT_DISCONNECTED:
            DcsWifiClientDisconnected();
            break;

        case LE_WIFICLIENT_EVENT_SCAN_DONE:
            if (!DcsWifiScan.isActive)
            {
                LE_DEBUG("Ignore irrelevant wifi scan result posting");
                break;
            }

            LE_DEBUG("Taking wifi scan result posting initiated by le_dcs");
            WifiReadScanResults();
            break;

        case LE_WIFICLIENT_EVENT_SCAN_FAILED:
            if (!DcsWifiScan.isActive)
            {
                LE_DEBUG("Ignore irrelevant wifi scan result posting");
                break;
            }

            LE_ERROR("Wifi scan failed to get results");
            DcsWifiScan.isActive = false;
            le_dcsTech_CollectChannelQueryResults(LE_DCS_TECH_WIFI, LE_FAULT, NULL, 0);
            DcsWifiClientStop();
            break;

        default:
            break;
    }
}


//--------------------------------------------------------------------------------------------------
/**
 * Wifi connection's disconnect retry timer handler
 * Upon the disconnect timer's expiration, check if further disconnect retry should proceed.
 * Go ahead if so.
 */
//--------------------------------------------------------------------------------------------------
static void DcsWifiDiscRetryTimerHandler
(
    le_timer_Ref_t timerRef
)
{
    uint16_t refcount;
    le_result_t ret;
    le_dcs_ChannelRef_t channelRef;
    le_dcs_WifiConnectionRef_t wifiConnRef = le_timer_GetContextPtr(timerRef);
    wifi_connDb_t *selectedConnDb, *wifiConnDb = DcsWifiGetDbFromRef(wifiConnRef);
    if (!wifiConnDb)
    {
        LE_ERROR("Failed to find wifi connection db from reference %p to retry disconnect",
                 wifiConnRef);
        return;
    }

    selectedConnDb = DcsWifiGetSelectedDb();
    if (!selectedConnDb)
    {
        LE_DEBUG("Wifi connection already disconnected upon retry timer expiration");
        return;
    }

    if (selectedConnDb != wifiConnDb)
    {
        LE_WARN("DCS has moved onto SSID %s from %s; skip retrying disconnect",
                selectedConnDb->ssid, wifiConnDb->ssid);
        return;
    }

    LE_INFO("Retry disconnect timer expired; initiate retry disconnecting over SSID %s now",
            wifiConnDb->ssid);

    if ((le_dcs_GetChannelRefCountFromTechRef(LE_DCS_TECH_WIFI, selectedConnDb->connRef, &refcount)
         == LE_OK) && (refcount > 0))
    {
        LE_DEBUG("Wifi connection in use again; no need to retry disconnecting");
        return;
    }

    ret = DcsWifiClientDisconnect(selectedConnDb);
    if ((ret == LE_OK) || (ret == LE_DUPLICATE))
    {
        return;
    }

    // Send the down event notification here as the Disconnect Request to wifiClient failed
    channelRef = le_dcs_GetChannelRefFromTechRef(LE_DCS_TECH_WIFI, selectedConnDb->connRef);
    if (!channelRef)
    {
        LE_ERROR("Failed to get channel db reference to send wifi connection down event");
    }
    else
    {
        le_dcs_ChannelEventNotifier(channelRef, LE_DCS_EVENT_DOWN);
    }
    DcsWifi.selectedConnDb = NULL;
    DcsWifi.apRef = NULL;
    DcsWifiClientStop();
}


//--------------------------------------------------------------------------------------------------
/**
 * Wifi connect's retry timer handler
 * Upon the connect timer's expiration, check if further connect retry should proceed. Go ahead if
 * so.
 */
//--------------------------------------------------------------------------------------------------
static void DcsWifiConnRetryTimerHandler
(
    le_timer_Ref_t timerRef
)
{
    le_result_t ret;
    le_dcs_ChannelRef_t channelRef;
    le_dcs_WifiConnectionRef_t wifiConnRef = le_timer_GetContextPtr(timerRef);
    wifi_connDb_t *selectedConnDb, *wifiConnDb = DcsWifiGetDbFromRef(wifiConnRef);
    if (!wifiConnDb)
    {
        LE_ERROR("Failed to find wifi connection db from reference %p to retry disconnect",
                 wifiConnRef);
        DcsWifiPostFailureReset();
        return;
    }

    channelRef = le_dcs_GetChannelRefFromTechRef(LE_DCS_TECH_WIFI,
                                                 wifiConnDb->connRef);
    if (!channelRef)
    {
        LE_ERROR("Failed to get channel db reference for wifi connection %s", wifiConnDb->ssid);
        DcsWifiPostFailureReset();
        return;
    }

    selectedConnDb = DcsWifiGetSelectedDb();
    if (!selectedConnDb)
    {
        LE_DEBUG("Wifi connection already disconnected upon connection retry timer expiration");
        le_dcs_ChannelEventNotifier(channelRef, LE_DCS_EVENT_DOWN);
        return;
    }

    if (selectedConnDb != wifiConnDb)
    {
        LE_WARN("DCS has moved onto SSID %s from %s; skip retrying connection",
                selectedConnDb->ssid, wifiConnDb->ssid);
        le_dcs_ChannelEventNotifier(channelRef, LE_DCS_EVENT_DOWN);
        return;
    }

    if (!DcsWifi.apRef)
    {
        LE_ERROR("Failed to retry connecting over SSID %s to an unknown AP reference",
                 wifiConnDb->ssid);
        le_dcs_ChannelEventNotifier(channelRef, LE_DCS_EVENT_DOWN);
        DcsWifiPostFailureReset();
        return;
    }

    LE_DEBUG("Connect retry timer expired; initiate retry connecting over SSID %s now",
             wifiConnDb->ssid);
    ret = le_wifiClient_Connect(DcsWifi.apRef);
    if (LE_OK == ret)
    {
        return;
    }

    LE_INFO("Failed to connect to AP with reference %p; error %d", DcsWifi.apRef, ret);

    // Retry connecting until exhausted
    ret = DcsWifiRetryConn(wifiConnDb);
    switch (ret)
    {
        case LE_OK:
            LE_INFO("Wait for the next retry before failing connection");
            return;
        case LE_DUPLICATE:
            LE_DEBUG("No need to re-trigger retry for connection");
            return;
        case LE_OVERFLOW:
        case LE_FAULT:
        default:
            break;
    }

    LE_INFO("Exhausted all attempts to connect over SSID %s", wifiConnDb->ssid);

    le_dcs_ChannelEventNotifier(channelRef, LE_DCS_EVENT_DOWN);
    DcsWifiPostFailureReset();
}


//--------------------------------------------------------------------------------------------------
/**
 * Wifi config loader
 * This function seeks to load up the necessary wifi configs before starting its connection which
 * include security protocol, user passphrase, etc.
 *
 * @return
 *     - LE_OK upon success in loading up all the necessary configs; otherwise some other
 *       le_result_t failure code
 */
//--------------------------------------------------------------------------------------------------
static le_result_t LoadWifiCfg
(
    const char *ssid
)
{
    le_wifiClient_AccessPointRef_t apRef;
    le_result_t ret = le_wifiClient_LoadSsid(ssid, &apRef);
    if (ret == LE_OK)
    {
        LE_DEBUG("Wifi configs installed to connect with AP reference %p over SSID %s",
                 apRef, ssid);
        DcsWifi.apRef = apRef;
    }
    else
    {
        LE_ERROR("Failed to install wifi configs to connect over SSID %s", ssid);
    }
    return ret;
}


//--------------------------------------------------------------------------------------------------
/**
 * Function for requesting wifi to start the given data/connection in the 1st argument
 *
 * @return
 *     - The function returns LE_OK or LE_DUPLICATE upon a successful start; otherwise, some other
 *       le_result_t failure cause
 */
//--------------------------------------------------------------------------------------------------
le_result_t le_dcsWifi_Start
(
    void *techRef                   ///< [IN] object reference of the cellular connection
)
{
    le_result_t ret;
    le_dcs_ChannelRef_t channelRef;
    le_dcs_WifiConnectionRef_t wifiConnRef = (le_dcs_WifiConnectionRef_t)techRef;
    wifi_connDb_t *wifiConnDb = DcsWifiGetDbFromRef(wifiConnRef);
    if (!wifiConnDb)
    {
        LE_ERROR("Failed to find wifi connection db from reference %p", techRef);
        return LE_FAULT;
    }

    channelRef = le_dcs_GetChannelRefFromTechRef(LE_DCS_TECH_WIFI, wifiConnRef);
    if (!channelRef)
    {
        LE_ERROR("Failed to get channel db reference for Wifi connection with reference %p",
                 wifiConnRef);
        return LE_FAULT;
    }

    if (DcsWifi.selectedConnDb)
    {
        if (DcsWifi.selectedConnDb->connRef == wifiConnRef)
        {
            // This code here is to catch corner cases, since the code in dcs.c is supposed
            // to have normally caught this case already
            LE_DEBUG("Wifi connection over SSID %s already started",
                     DcsWifi.selectedConnDb->ssid);
            return LE_DUPLICATE;
        }
        LE_ERROR("Failed to start connection with SSID %s while connection with SSID %s "
                 "already selected", wifiConnDb->ssid, DcsWifi.selectedConnDb->ssid);
        le_dcs_ChannelEventNotifier(channelRef, LE_DCS_EVENT_DOWN);
        return LE_FAULT;
    }

    if (DcsWifiScan.isActive)
    {
        LE_ERROR("Starting a wifi connection disallowed while wifi scanning is in progress");
        le_dcs_ChannelEventNotifier(channelRef, LE_DCS_EVENT_DOWN);
        return LE_NOT_PERMITTED;
    }

    if (LE_OK != LoadWifiCfg(wifiConnDb->ssid))
    {
        LE_ERROR("Failed to load wifi config to start connection");
        le_dcs_ChannelEventNotifier(channelRef, LE_DCS_EVENT_DOWN);
        return LE_FAULT;
    }

    // Start Wifi client
    ret = DcsWifiClientStart();
    if ((LE_OK != ret) && (LE_BUSY != ret) && (LE_DUPLICATE != ret))
    {
        LE_ERROR("Failed to start wifi; error %d", ret);
        le_dcs_ChannelEventNotifier(channelRef, LE_DCS_EVENT_DOWN);
        return LE_FAULT;
    }

    // As it can proceed to try connecting, stop the disconnect retry timer if it's running
    if (le_timer_IsRunning(DcsWifi.discRetryTimer))
    {
        le_timer_Stop(DcsWifi.discRetryTimer);
    }

    DcsWifi.discRetries = 0;
    DcsWifi.discRetryBackoff = WIFI_DISC_RETRY_BACKOFF_INIT;
    DcsWifi.connRetries = 0;
    DcsWifi.connRetryBackoff = WIFI_CONN_RETRY_BACKOFF_INIT;

    // Connect to the Access Point
    DcsWifi.selectedConnDb = wifiConnDb;
    LE_INFO("Connecting Wifi client to over SSID %s to AP with reference %p",
            DcsWifi.selectedConnDb->ssid, DcsWifi.apRef);
    ret = le_wifiClient_Connect(DcsWifi.apRef);
    if (LE_OK != ret)
    {
        LE_INFO("Failed to connect to AP with reference %p; error %d", DcsWifi.apRef, ret);

        // Retry connecting until exhausted
        ret = DcsWifiRetryConn(DcsWifi.selectedConnDb);
        switch (ret)
        {
            case LE_OK:
                LE_INFO("Wait for the next retry before failing connection");
                return LE_OK;
            case LE_DUPLICATE:
                LE_DEBUG("No need to re-trigger retry for connection");
                return LE_OK;
            case LE_OVERFLOW:
            case LE_FAULT:
            default:
                break;
        }

        LE_INFO("Exhausted all attempts to connect over SSID %s", DcsWifi.selectedConnDb->ssid);

        le_dcs_ChannelEventNotifier(channelRef, LE_DCS_EVENT_DOWN);
        DcsWifiPostFailureReset();
    }

    return ret;
}


//--------------------------------------------------------------------------------------------------
/**
 * Function for stopping the given data link/connection in the argument
 *
 * @return
 *     - The function returns LE_OK upon a successful stop; otherwise, some other
 *       le_result_t failure cause
 */
//--------------------------------------------------------------------------------------------------
le_result_t le_dcsWifi_Stop
(
    void *techRef                   ///< [IN] object reference of the cellular connection
)
{
    le_result_t ret;
    le_dcs_ChannelRef_t channelRef;
    le_dcs_WifiConnectionRef_t wifiConnRef = (le_dcs_WifiConnectionRef_t)techRef;
    wifi_connDb_t *selectedConnDb, *wifiConnDb = DcsWifiGetDbFromRef(wifiConnRef);
    if (!wifiConnDb)
    {
        LE_ERROR("Failed to find wifi connection db from reference %p", techRef);
        return LE_FAULT;
    }

    selectedConnDb = DcsWifiGetSelectedDb();
    if (!selectedConnDb)
    {
        LE_INFO("Wifi connection already stopped");
        return LE_DUPLICATE;
    }

    if (selectedConnDb->connRef != wifiConnRef)
    {
        LE_ERROR("Unable to stop wifi connection over SSID %s which DCS hasn't started",
                 wifiConnDb->ssid);
        return LE_FAULT;
    }

    if (le_timer_IsRunning(DcsWifi.connRetryTimer))
    {
        le_timer_Stop(DcsWifi.connRetryTimer);
    }

    ret = DcsWifiClientDisconnect(selectedConnDb);
    if ((ret != LE_OK) && (ret != LE_DUPLICATE))
    {
        // Send the down event notification here as the Disconnect Request to wifiClient failed
        channelRef = le_dcs_GetChannelRefFromTechRef(LE_DCS_TECH_WIFI, wifiConnRef);
        if (!channelRef)
        {
            LE_ERROR("Failed to get channel db reference to send wifi connection down event");
        }
        else
        {
            le_dcs_ChannelEventNotifier(channelRef, LE_DCS_EVENT_DOWN);
        }
        DcsWifi.selectedConnDb = NULL;
        DcsWifi.apRef = NULL;
        DcsWifiClientStop();
    }

    // Wait till the Disconnected event from wifiClient before sending the Down event notification
    return ret;
}


//--------------------------------------------------------------------------------------------------
/**
 *  Wifi handlers component initialization
 */
//--------------------------------------------------------------------------------------------------
COMPONENT_INIT
{
    // Init DCS's wifi data structures
    memset(&DcsWifiScan, 0, sizeof(DcsWifiScan));
    memset(&DcsWifi, 0, sizeof(DcsWifi));
    DcsWifi.dbList = LE_DLS_LIST_INIT;

    // Allocate the connection DB app event pool, and set the max number of objects
    WifiConnDbPool = le_mem_CreatePool("WifiConnDbPool", sizeof(wifi_connDb_t));
    le_mem_ExpandPool(WifiConnDbPool, WIFI_CONNDBS_MAX);
    le_mem_SetDestructor(WifiConnDbPool, DcsWifiConnDbDestructor);

    // Create a safe reference map for wifi connection objects
    WifiConnectionRefMap = le_ref_CreateMap("Wifi Connection Reference Map", WIFI_CONNDBS_MAX);

    DcsWifi.eventHandlerRef = le_wifiClient_AddNewEventHandler(WifiClientEventHandler, NULL);
    if (!DcsWifi.eventHandlerRef)
    {
        LE_ERROR("Failed to add Wifi event handler");
    }

    // Init disconnect retry timer
    DcsWifi.discRetryTimer = le_timer_Create("WifiDiscRetryTimer");
    if ((LE_OK != le_timer_SetHandler(DcsWifi.discRetryTimer, DcsWifiDiscRetryTimerHandler))
        || (LE_OK != le_timer_SetRepeat(DcsWifi.discRetryTimer, 1)))
    {
        LE_ERROR("Failed to init disconnect retry timer for wifi connection");
    }

    // Init connection retry timer & backoff parameters
    DcsWifi.connRetryTimer = le_timer_Create("WifiConnRetryTimer");
    if ((LE_OK != le_timer_SetHandler(DcsWifi.connRetryTimer, DcsWifiConnRetryTimerHandler))
        || (LE_OK != le_timer_SetRepeat(DcsWifi.connRetryTimer, 1)))
    {
        LE_ERROR("Failed to init connect retry timer for wifi connection");
    }

    LE_INFO("Data Channel Service's Wifi component is ready");
}
