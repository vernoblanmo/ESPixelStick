/*
* E131Input.cpp - Code to wrap ESPAsyncE131 for input
*
* Project: ESPixelStick - An ESP8266 / ESP32 and E1.31 based pixel driver
* Copyright (c) 2021 Shelby Merrick
* http://www.forkineye.com
*
*  This program is provided free for you to use in any way that you wish,
*  subject to the laws and regulations where you are using it.  Due diligence
*  is strongly suggested before using this code.  Please give credit where due.
*
*  The Author makes no warranty of any kind, express or implied, with regard
*  to this program or the documentation contained in this document.  The
*  Author shall not be liable in any event for incidental or consequential
*  damages in connection with, or arising out of, the furnishing, performance
*  or use of these programs.
*
*/

#include "InputE131.hpp"
#include "../WiFiMgr.hpp"

//-----------------------------------------------------------------------------
c_InputE131::c_InputE131 (c_InputMgr::e_InputChannelIds NewInputChannelId,
                          c_InputMgr::e_InputType       NewChannelType,
                          uint8_t                     * BufferStart,
                          uint16_t                      BufferSize) :
    c_InputCommon(NewInputChannelId, NewChannelType, BufferStart, BufferSize)

{
    // DEBUG_START;
    // DEBUG_V ("BufferSize: " + String (BufferSize));
    // DEBUG_END;
} // c_InputE131

//-----------------------------------------------------------------------------
c_InputE131::~c_InputE131()
{
    // DEBUG_START;

    // The E1.31 layer and UDP layer do not handle a shut down well (at all). Ask for a reboot.
    LOG_PORT.println (String (F ("** 'E1.31' Shut Down for input: '")) + String (InputChannelId) + String (F ("' Requires a reboot. **")));

    // DEBUG_END;

} // ~c_InputE131

//-----------------------------------------------------------------------------
void c_InputE131::Begin ()
{
    // DEBUG_START;

    do // once
    {
        if (true == HasBeenInitialized)
        {
            // DEBUG_V ("");
            // break;
        }

        // DEBUG_V ("InputDataBufferSize: " + String(InputDataBufferSize));

        validateConfiguration ();
        // DEBUG_V ("");

        NetworkStateChanged (WiFiMgr.IsWiFiConnected (), false);

        // DEBUG_V ("");
        HasBeenInitialized = true;

    } while (false);

    // DEBUG_END;

} // Begin

//-----------------------------------------------------------------------------
void c_InputE131::GetConfig (JsonObject & jsonConfig)
{
    // DEBUG_START;

    jsonConfig[CN_universe]       = startUniverse;
    jsonConfig[CN_universe_limit] = ChannelsPerUniverse;
    jsonConfig[CN_universe_start] = FirstUniverseChannelOffset;

    // DEBUG_END;

} // GetConfig

//-----------------------------------------------------------------------------
void c_InputE131::GetStatus (JsonObject & jsonStatus)
{
    // DEBUG_START;

    JsonObject e131Status = jsonStatus.createNestedObject (F ("e131"));
    e131Status["unifirst"]      = startUniverse;
    e131Status["unilast"]       = LastUniverse;
    e131Status["unichanlim"]    = ChannelsPerUniverse;
    // DEBUG_V ("");

    e131Status["num_packets"]   = e131->stats.num_packets;
    e131Status["packet_errors"] = e131->stats.packet_errors;
    e131Status["last_clientIP"] = e131->stats.last_clientIP.toString ();

    // DEBUG_END;

} // GetStatus

//-----------------------------------------------------------------------------
void c_InputE131::Process ()
{
    // DEBUG_START;

    uint8_t*    E131Data;
    uint8_t     AdjustedUniverseIndex;
    uint16_t    CurrentUniverse;

    // Parse a packet and update pixels
    while (!e131->isEmpty ())
    {
        e131->pull (&packet);
        CurrentUniverse = ntohs (packet.universe);
        E131Data = packet.property_values + 1;

        // DEBUG_V ("              universe: " + String(universe));
        // DEBUG_V ("packet.sequence_number: " + String(packet.sequence_number));

        if ((CurrentUniverse < startUniverse) || (CurrentUniverse > LastUniverse))
        {
            DEBUG_V ("Not interested in this universe");
            continue;
        }

        // Universe offset and sequence tracking
        AdjustedUniverseIndex = (CurrentUniverse - startUniverse);
        
        // Do we need to update a sequnce error?
        if (packet.sequence_number != seqTracker[AdjustedUniverseIndex]++)
        {
            LOG_PORT.print (F ("Sequence Error - expected: "));
            LOG_PORT.print (seqTracker[AdjustedUniverseIndex] - 1);
            LOG_PORT.print (F (" actual: "));
            LOG_PORT.print (packet.sequence_number);
            LOG_PORT.print (String (CN_universe) + ":");
            LOG_PORT.println (CurrentUniverse);
            seqError[AdjustedUniverseIndex]++;
            seqTracker[AdjustedUniverseIndex] = packet.sequence_number + 1;
        }

        uint16_t NumBytesOfE131Data = ntohs (packet.property_value_count) - 1;
        UniverseIdToBufferXlate_t & CurrentTranslationEntry = UniverseIdToBufferXlate[AdjustedUniverseIndex];
        memcpy (CurrentTranslationEntry.Destination,
                &E131Data[CurrentTranslationEntry.SourceDataOffset],
                min(CurrentTranslationEntry.BytesToCopy, NumBytesOfE131Data) );

        InputMgr.ResetBlankTimer ();

    } // end while there is data to process

    // DEBUG_END;

} // process

//-----------------------------------------------------------------------------
void c_InputE131::SetBufferInfo (uint8_t* BufferStart, uint16_t BufferSize)
{
    // DEBUG_START;

    InputDataBuffer = BufferStart;
    InputDataBufferSize = BufferSize;

    // buffer has moved. Start Over
    HasBeenInitialized = false;
    Begin ();

    SetBufferTranslation ();

    // DEBUG_END;

} // SetBufferInfo

//-----------------------------------------------------------------------------
boolean c_InputE131::SetConfig (ArduinoJson::JsonObject& jsonConfig)
{
    // DEBUG_START;

    setFromJSON (startUniverse,              jsonConfig, CN_universe);
    setFromJSON (ChannelsPerUniverse,        jsonConfig, CN_universe_limit);
    setFromJSON (FirstUniverseChannelOffset, jsonConfig, CN_universe_start);

    validateConfiguration ();

    // Update the config fields in case the validator changed them
    GetConfig (jsonConfig);

    // DEBUG_END;
    return true;
} // SetConfig

//-----------------------------------------------------------------------------
// Subscribe to "n" universes, starting at "universe"
void c_InputE131::SubscribeToMulticastDomains()
{
    uint8_t count = LastUniverse - startUniverse + 1;
    IPAddress ifaddr = WiFi.localIP ();
    IPAddress multicast_addr;

    for (uint8_t UniverseIndex = 0; UniverseIndex < count; ++UniverseIndex)
    {
        multicast_addr = IPAddress (239, 255,
                                    (((startUniverse + UniverseIndex) >> 8) & 0xff),
                                    (((startUniverse + UniverseIndex) >> 0) & 0xff));

        igmp_joingroup ((ip4_addr_t*)&ifaddr[0], (ip4_addr_t*)&multicast_addr[0]);
    }
} // multiSub

//-----------------------------------------------------------------------------
void c_InputE131::validateConfiguration ()
{
    // DEBUG_START;

    if (startUniverse < 1) { startUniverse = 1; }

    // DEBUG_V ("");
    if (ChannelsPerUniverse > UNIVERSE_MAX || ChannelsPerUniverse < 1) { ChannelsPerUniverse = UNIVERSE_MAX; }

    // DEBUG_V ("");
    if (FirstUniverseChannelOffset < 1)
    {
        // move to the start of the first universe
        FirstUniverseChannelOffset = 1;
    }
    else if (FirstUniverseChannelOffset > ChannelsPerUniverse)
    {
        // channel start must be within the first universe
        FirstUniverseChannelOffset = ChannelsPerUniverse;
    }

    // Find the last universe we should listen for
     // DEBUG_V ("");
    uint16_t span = FirstUniverseChannelOffset + InputDataBufferSize - 1;
    if (span % ChannelsPerUniverse)
    {
        LastUniverse = startUniverse + span / ChannelsPerUniverse;
    }
    else
    {
        LastUniverse = startUniverse + span / ChannelsPerUniverse - 1;
    }

    // Setup the sequence error tracker
    // DEBUG_V ("");
    uint8_t NumberOfUniversesToProccess = (LastUniverse + 1) - startUniverse;

    // DEBUG_V ("");
    if (seqTracker) { free (seqTracker); seqTracker = nullptr; }
    // DEBUG_V ("");
    if ((seqTracker = static_cast<uint8_t*>(malloc (NumberOfUniversesToProccess)))) { memset (seqTracker, 0x00, NumberOfUniversesToProccess); }
    // DEBUG_V ("");

    if (seqError) { free (seqError); seqError = nullptr; }
    // DEBUG_V ("");
    if ((seqError = static_cast<uint32_t*>(malloc (NumberOfUniversesToProccess * 4)))) { memset (seqError, 0x00, NumberOfUniversesToProccess * 4); }
    // DEBUG_V ("");

    SetBufferTranslation ();

    // Zero out packet stats
    if (nullptr == e131)
    {
        // DEBUG_V ("");
        e131 = new ESPAsyncE131 (10);
    }
    // DEBUG_V ("");
    e131->stats.num_packets = 0;

    // DEBUG_END;

} // validateConfiguration

//-----------------------------------------------------------------------------
void c_InputE131::SetBufferTranslation ()
{
    DEBUG_START;

    memset ((void*)UniverseIdToBufferXlate, 0x00, sizeof (UniverseIdToBufferXlate));

    // for each possible universe, set the start and size

    // uint16_t    ChannelsPerUniverse        = 512;  ///< Universe boundary limit
    // uint16_t    FirstUniverseChannelOffset = 1;    ///< Channel to start listening at - 1 based

    uint16_t InputOffset = FirstUniverseChannelOffset - 1;
    uint16_t DestinationOffset = 0;

    // set up the bytes for the First Universe
    int BytesInUniverse = ChannelsPerUniverse - InputOffset;

    for (UniverseIdToBufferXlate_t CurrentTranslation : UniverseIdToBufferXlate)
    {
        CurrentTranslation.Destination      = &InputDataBuffer[DestinationOffset];
        CurrentTranslation.BytesToCopy      = BytesInUniverse;
        CurrentTranslation.SourceDataOffset = InputOffset;

        DestinationOffset += BytesInUniverse;

        BytesInUniverse = ChannelsPerUniverse;
        InputOffset = 0;
    }

    // DEBUG_V ("");

    DEBUG_END;

} // SetBufferTranslation

//-----------------------------------------------------------------------------
void c_InputE131::NetworkStateChanged (bool IsConnected)
{
    NetworkStateChanged (IsConnected, true);
} // NetworkStateChanged

//-----------------------------------------------------------------------------
void c_InputE131::NetworkStateChanged (bool IsConnected, bool ReBootAllowed)
{
    // DEBUG_START;

    if (nullptr == e131)
    {
        // DEBUG_V ("Instantiate E1.31");
        e131 = new ESPAsyncE131 (10);
    }
    // DEBUG_V ("");

    if (IsConnected)
    {
        // Get on with business
        if (e131->begin (E131_MULTICAST, startUniverse, LastUniverse - startUniverse + 1))
        {
            LOG_PORT.println (F ("E1.31 Multicast Enabled."));
        }
        else
        {
            LOG_PORT.println (F ("*** E1.31 MULTICAST INIT FAILED ****"));
        }

        // DEBUG_V ("");

        if (e131->begin (E131_UNICAST))
        {
            LOG_PORT.println (String (F ("E1.31 Unicast Enabled on port: ")) + E131_DEFAULT_PORT);
        }
        else
        {
            LOG_PORT.println (F ("*** E1.31 UNICAST INIT FAILED ****"));
        }

        LOG_PORT.printf_P (PSTR ("Listening for %u channels from Universe %u to %u.\n"),
            InputDataBufferSize, startUniverse, LastUniverse);

        // Setup IGMP subscriptions if multicast is enabled
        SubscribeToMulticastDomains ();
    }
    else if (ReBootAllowed)
    {
        // handle a disconnect
        // E1.31 does not do this gracefully. A loss of connection needs a reboot
        extern bool reboot;
        reboot = true;
        LOG_PORT.println (F ("E1.31 Input requesting reboot on loss of WiFi connection."));
    }

    // DEBUG_END;

} // NetworkStateChanged
