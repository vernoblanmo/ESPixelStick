/*
* OutputWS2801Spi.cpp - WS2801 driver code for ESPixelStick SPI Channel
*
* Project: ESPixelStick - An ESP8266 / ESP32 and E1.31 based pixel driver
* Copyright (c) 2015 Shelby Merrick
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

#include "../ESPixelStick.h"
#ifdef ARDUINO_ARCH_ESP32
#include "OutputWS2801Spi.hpp"
#include "driver/spi_master.h"
#include <esp_heap_alloc_caps.h>

//----------------------------------------------------------------------------
/* shell function to set the 'this' pointer of the real ISR
   This allows me to use non static variables in the ISR.
 */
static void IRAM_ATTR ws2801_transfer_callback (spi_transaction_t * param)
{
    reinterpret_cast <c_OutputWS2801Spi*>(param->user)->DataCbCounter++;
    vTaskResume (reinterpret_cast <c_OutputWS2801Spi*>(param->user)->GetTaskHandle());
} // ws2801_transfer_callback

//----------------------------------------------------------------------------
static void SendIntensityDataTask (void* pvParameters)
{
    // DEBUG_START; Needs extra stack space to run this
    do
    {
        // we start suspended
        vTaskSuspend (NULL); //Suspend Own Task
        reinterpret_cast <c_OutputWS2801Spi*>(pvParameters)->DataTaskcounter++;
        reinterpret_cast <c_OutputWS2801Spi*>(pvParameters)->SendIntensityData ();

    } while (true);
    // DEBUG_END;

} // SendIntensityDataTask

//----------------------------------------------------------------------------
c_OutputWS2801Spi::c_OutputWS2801Spi (c_OutputMgr::e_OutputChannelIds OutputChannelId,
    gpio_num_t outputGpio,
    uart_port_t uart,
    c_OutputMgr::e_OutputType outputType) :
    c_OutputWS2801 (OutputChannelId, outputGpio, uart, outputType)
{
    // DEBUG_START;

    // update frame calculation 
    BlockSize = WS2801_NUM_INTENSITY_PER_TRANSACTION;
    BlockDelay = 20.0; // measured between 16 and 21 us

    // DEBUG_END;
} // c_OutputWS2801Spi

//----------------------------------------------------------------------------
c_OutputWS2801Spi::~c_OutputWS2801Spi ()
{
    // DEBUG_START;
    if (gpio_num_t (-1) == DataPin) { return; }

    Shutdown ();

    if (SendIntensityDataTaskHandle)
    {
        vTaskDelete (SendIntensityDataTaskHandle);
    }

    // DEBUG_END;

} // ~c_OutputWS2801Spi

//----------------------------------------------------------------------------
void c_OutputWS2801Spi::Shutdown ()
{
    // DEBUG_START;

    if (spi_device_handle)
    {
        // DEBUG_V ();
        for (auto & Transaction : Transactions)
        {
            // DEBUG_V ();
            spi_transaction_t * temp = & Transaction;
            spi_device_get_trans_result (spi_device_handle, &temp, portMAX_DELAY);
            // DEBUG_V ();
        }

        // DEBUG_V ();
        for (auto & TransactionBuffer : TransactionBuffers)
        {
            // DEBUG_V ();
            if (TransactionBuffer)
            {
                // DEBUG_V ();
                free (TransactionBuffer);
                // DEBUG_V ();
                TransactionBuffer = nullptr;
                // DEBUG_V ();
            }
        }
        // DEBUG_V ();

        spi_device_release_bus (spi_device_handle);
        // DEBUG_V ();
        spi_bus_remove_device (spi_device_handle);
        // DEBUG_V ();
    }

    spi_bus_free (VSPI_HOST);

    // DEBUG_END;

} // Shutdown

//----------------------------------------------------------------------------
/* Use the current config to set up the output port
*/
void c_OutputWS2801Spi::Begin ()
{
    // DEBUG_START;

    spi_bus_config_t SpiBusConfiguration;
    memset ((void*)&SpiBusConfiguration, 0x00, sizeof (SpiBusConfiguration));
    SpiBusConfiguration.miso_io_num = -1;
    SpiBusConfiguration.mosi_io_num = DataPin;
    SpiBusConfiguration.sclk_io_num = ClockPin;
    SpiBusConfiguration.quadwp_io_num = -1;
    SpiBusConfiguration.quadhd_io_num = -1;
    SpiBusConfiguration.max_transfer_sz = OM_MAX_NUM_CHANNELS;
    SpiBusConfiguration.flags = SPICOMMON_BUSFLAG_MASTER;

    spi_device_interface_config_t SpiDeviceConfiguration;
    memset ((void*)&SpiDeviceConfiguration, 0x00, sizeof (SpiDeviceConfiguration));
    // SpiDeviceConfiguration.command_bits = 0; // No command to send
    // SpiDeviceConfiguration.address_bits = 0; // No bus address to send
    // SpiDeviceConfiguration.dummy_bits = 0; // No dummy bits to send
    // SpiDeviceConfiguration.duty_cycle_pos = 0; // 50% Duty cycle
    SpiDeviceConfiguration.clock_speed_hz = WS2801_SPI_MASTER_FREQ_1M;
    SpiDeviceConfiguration.mode = 0;                                // SPI mode 0
    SpiDeviceConfiguration.spics_io_num = -1;                       // we will NOT use CS pin
    SpiDeviceConfiguration.queue_size = 10*WS2801_NUM_TRANSACTIONS;    // We want to be able to queue 2 transactions at a time
    // SpiDeviceConfiguration.pre_cb = nullptr;                     // Specify pre-transfer callback to handle D/C line
    SpiDeviceConfiguration.post_cb = ws2801_transfer_callback;      // Specify post-transfer callback to handle D/C line
    // SpiDeviceConfiguration.flags = 0;

    ESP_ERROR_CHECK (spi_bus_initialize (WS2801_SPI_HOST, &SpiBusConfiguration, WS2801_SPI_DMA_CHANNEL));
    ESP_ERROR_CHECK (spi_bus_add_device (WS2801_SPI_HOST, &SpiDeviceConfiguration, &spi_device_handle));
    ESP_ERROR_CHECK (spi_device_acquire_bus (spi_device_handle, portMAX_DELAY));

    NextTransactionToFill = 0;
    for (auto & TransactionBufferToSet : TransactionBuffers)
    {
        TransactionBuffers[NextTransactionToFill] = (byte*)pvPortMallocCaps (WS2801_NUM_INTENSITY_PER_TRANSACTION, MALLOC_CAP_DMA); ///< Pointer to transmit buffer
        // DEBUG_V (String ("tx_buffer: 0x") + String (uint32_t(TransactionBuffers[NextTransactionToFill]), HEX));
        NextTransactionToFill++;
    }

    NextTransactionToFill = 0;

    xTaskCreate (SendIntensityDataTask, "WS2801Task", 1000, this, ESP_TASK_PRIO_MIN + 4, &SendIntensityDataTaskHandle);
    // xTaskCreate (SendIntensityDataTask, "WS2801Task", 3000, this, ESP_TASK_PRIO_MIN + 2, &SendIntensityDataTaskHandle);

    // DEBUG_END;

} // init

//----------------------------------------------------------------------------
void c_OutputWS2801Spi::GetConfig (ArduinoJson::JsonObject& jsonConfig)
{
    // DEBUG_START;

    c_OutputWS2801::GetConfig (jsonConfig);

    jsonConfig[CN_clock_pin] = ClockPin;

    // DEBUG_END;
} // GetConfig

//----------------------------------------------------------------------------
void c_OutputWS2801Spi::GetStatus (ArduinoJson::JsonObject& jsonStatus)
{
    c_OutputWS2801::GetStatus (jsonStatus);

    // jsonStatus["FrameStartCounter"] = FrameStartCounter;
    jsonStatus["SendIntensityDataCounter"] = SendIntensityDataCounter;
    jsonStatus["DataTaskcounter"] = DataTaskcounter;
    jsonStatus["DataCbCounter"] = DataCbCounter;
    
} // GetStatus

//----------------------------------------------------------------------------
bool c_OutputWS2801Spi::SetConfig (ArduinoJson::JsonObject& jsonConfig)
{
    // DEBUG_START;

    gpio_num_t OldDataPin = DataPin;
    gpio_num_t OldClockPin = ClockPin;

    bool response = c_OutputWS2801::SetConfig (jsonConfig);
    
    setFromJSON (ClockPin, jsonConfig, CN_clock_pin);
    
    if ((OldDataPin != DataPin) || (OldClockPin != ClockPin))
    {
        // DEBUG_V (String ("start over."));
        Shutdown ();
        Begin ();
    }

    // DEBUG_END;
    return response;

} // GetStatus

//----------------------------------------------------------------------------
void c_OutputWS2801Spi::SendIntensityData ()
{
    // DEBUG_START;
    SendIntensityDataCounter++;

    if (MoreDataToSend)
    {
        spi_transaction_t& TransactionToFill = Transactions[NextTransactionToFill];
        memset ((void*)&Transactions[NextTransactionToFill], 0x00, sizeof (spi_transaction_t));

        TransactionToFill.user = this;         ///< User-defined variable. Can be used to store eg transaction ID.
        byte* pMem = &TransactionBuffers[NextTransactionToFill][0];
        TransactionToFill.tx_buffer = pMem;
        uint32_t NumEmptyIntensitySlots = WS2801_NUM_INTENSITY_PER_TRANSACTION;

        while ((NumEmptyIntensitySlots) && (MoreDataToSend))
        {
            *pMem++ = GetNextIntensityToSend ();
            --NumEmptyIntensitySlots;
        } // end while there is space in the buffer

        TransactionToFill.length = WS2801_BITS_PER_INTENSITY * (WS2801_NUM_INTENSITY_PER_TRANSACTION - NumEmptyIntensitySlots);
        if (!MoreDataToSend)
        {
            TransactionToFill.length++;
        }

        if (++NextTransactionToFill >= WS2801_NUM_TRANSACTIONS)
        {
            NextTransactionToFill = 0;
        }

        ESP_ERROR_CHECK (spi_device_queue_trans (spi_device_handle, &TransactionToFill, portMAX_DELAY));
    }

    // DEBUG_END;

} // SendIntensityData

//----------------------------------------------------------------------------
void c_OutputWS2801Spi::Render ()
{
    // DEBUG_START;

    if (canRefresh ())
    {
        StartNewFrame ();
        ReportNewFrame ();

        // fill all the available buffers
        NextTransactionToFill = 0;
        for (auto & TransactionToFill : Transactions)
        {
            SendIntensityData ();
        }
    }

    // DEBUG_END;

} // render

#endif // def ARDUINO_ARCH_ESP32
