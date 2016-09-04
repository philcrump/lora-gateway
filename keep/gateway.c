#include <features.h>
#define __USE_XOPEN
#include <stdio.h>
#include <stdio.h>
#include <curl/curl.h>
#include <stdlib.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <errno.h>
#include <stdint.h>
#include <stdarg.h>
#include <pthread.h>
#include <curses.h>
#include <math.h>
#include <dirent.h>
#include <wiringPi.h>
#include <wiringPiSPI.h>
#include <time.h>

#include "urlencode.h"
#include "base64.h"
#include "ssdv.h"
#include "ftp.h"
#include "habitat.h"
#include "network.h"
#include "global.h"
#include "server.h"
#include "gateway.h"

#define VERSION	"V1.8.1"
bool run = TRUE;

// RFM98
uint8_t currentMode = 0x81;

#define REG_FIFO                    0x00
#define REG_FIFO_ADDR_PTR           0x0D
#define REG_FIFO_TX_BASE_AD         0x0E
#define REG_FIFO_RX_BASE_AD         0x0F
#define REG_RX_NB_BYTES             0x13
#define REG_OPMODE                  0x01
#define REG_FIFO_RX_CURRENT_ADDR    0x10
#define REG_IRQ_FLAGS               0x12
#define REG_PACKET_SNR				0x19
#define REG_PACKET_RSSI				0x1A
#define REG_CURRENT_RSSI			0x1B
#define REG_DIO_MAPPING_1           0x40
#define REG_DIO_MAPPING_2           0x41
#define REG_MODEM_CONFIG            0x1D
#define REG_MODEM_CONFIG2           0x1E
#define REG_MODEM_CONFIG3           0x26
#define REG_PAYLOAD_LENGTH          0x22
#define REG_IRQ_FLAGS_MASK          0x11
#define REG_HOP_PERIOD              0x24
#define REG_FREQ_ERROR				0x28
#define REG_DETECT_OPT				0x31
#define	REG_DETECTION_THRESHOLD		0x37

// MODES
#define RF98_MODE_RX_CONTINUOUS     0x85
#define RF98_MODE_TX                0x83
#define RF98_MODE_SLEEP             0x80
#define RF98_MODE_STANDBY           0x81

#define PAYLOAD_LENGTH              255

// Modem Config 1
#define EXPLICIT_MODE               0x00
#define IMPLICIT_MODE               0x01

#define ERROR_CODING_4_5            0x02
#define ERROR_CODING_4_6            0x04
#define ERROR_CODING_4_7            0x06
#define ERROR_CODING_4_8            0x08

#define BANDWIDTH_7K8               0x00
#define BANDWIDTH_10K4              0x10
#define BANDWIDTH_15K6              0x20
#define BANDWIDTH_20K8              0x30
#define BANDWIDTH_31K25             0x40
#define BANDWIDTH_41K7              0x50
#define BANDWIDTH_62K5              0x60
#define BANDWIDTH_125K              0x70
#define BANDWIDTH_250K              0x80
#define BANDWIDTH_500K              0x90

// Modem Config 2

#define SPREADING_6                 0x60
#define SPREADING_7                 0x70
#define SPREADING_8                 0x80
#define SPREADING_9                 0x90
#define SPREADING_10                0xA0
#define SPREADING_11                0xB0
#define SPREADING_12                0xC0

#define CRC_OFF                     0x00
#define CRC_ON                      0x04

// POWER AMPLIFIER CONFIG
#define REG_PA_CONFIG               0x09
#define PA_MAX_BOOST                0x8F
#define PA_LOW_BOOST                0x81
#define PA_MED_BOOST                0x8A
#define PA_MAX_UK                   0x88
#define PA_OFF_BOOST                0x00
#define RFO_MIN                     0x00

// LOW NOISE AMPLIFIER
#define REG_LNA                     0x0C
#define LNA_MAX_GAIN                0x23    // 0010 0011
#define LNA_OFF_GAIN                0x00
#define LNA_LOW_GAIN                0xC0    // 1100 0000

struct TPayload {
    int InUse;
    char Payload[32];
};

struct TLoRaMode 
{
	int	ImplicitOrExplicit;
	int ErrorCoding;
	int Bandwidth;
	int SpreadingFactor;
	int LowDataRateOptimize;
	int BaudRate;
	char *Description;
} LoRaModes[] =
{
	{EXPLICIT_MODE, ERROR_CODING_4_8, BANDWIDTH_20K8, SPREADING_11, 8,    60, "Telemetry"},			// 0: Normal mode for telemetry
	{IMPLICIT_MODE, ERROR_CODING_4_5, BANDWIDTH_20K8, SPREADING_6,  0,  1400, "SSDV"},				// 1: Normal mode for SSDV
	{EXPLICIT_MODE, ERROR_CODING_4_8, BANDWIDTH_62K5, SPREADING_8,  0,  2000, "Repeater"},			// 2: Normal mode for repeater network	
	{EXPLICIT_MODE, ERROR_CODING_4_6, BANDWIDTH_250K, SPREADING_7,  0,  8000, "Turbo"},				// 3: Normal mode for high speed images in 868MHz band
	{IMPLICIT_MODE, ERROR_CODING_4_5, BANDWIDTH_250K, SPREADING_6,  0, 16828, "TurboX"},			// 4: Fastest mode within IR2030 in 868MHz band
	{EXPLICIT_MODE, ERROR_CODING_4_8, BANDWIDTH_41K7, SPREADING_11, 0,   200, "Calling"},			// 5: Calling mode
	{IMPLICIT_MODE, ERROR_CODING_4_5, BANDWIDTH_41K7, SPREADING_6,  0,  2800, "Uplink"}				// 6: Uplink mode for 868
};

struct TConfig Config;
struct TPayload Payloads[16];

int LEDCounts[2];
pthread_mutex_t var = PTHREAD_MUTEX_INITIALIZER;

#pragma pack(1)

struct TBinaryPacket {
    uint8_t PayloadIDs;
    uint16_t Counter;
    uint16_t BiSeconds;
    float Latitude;
    float Longitude;
    uint16_t Altitude;
};

const char *Modes[6] =
    { "Slow", "SSDV", "Repeater", "Turbo", "TurboX", "Calling" };

// Create pipes for inter proces communication 
// GLOBAL AS CALLED FROM INTERRRUPT
int telem_pipe_fd[2];
int ssdv_pipe_fd[2];

// Create a structure to share some variables with the habitat child process
// GLOBAL AS CALLED FROM INTERRRUPT
thread_shared_vars_t htsv;

// Create a structure to share some variables with the ssdv child process
// GLOBAL AS CALLED FROM INTERRRUPT
thread_shared_vars_t stsv;

int habitate_telem_packets = 0;

void
hexdump_buffer( const char *title, const char *buffer, const int len_buffer )
{
    int i, j = 0;
    char message[200];
    FILE *fp;

    fp = fopen( "pkt.txt", "a" );

    fprintf( fp, "Title = %s\n", title );

    for ( i = 0; i < len_buffer; i++ )
    {
        sprintf( &message[3 * j], "%02x ", buffer[i] );
        j++;
        if ( i % 16 == 15 )
        {
            j = 0;
            fprintf( fp, "%s\n", message );
            message[0] = '\0';
        }
    }
    fprintf( fp, "%s\n", message );
    fclose( fp );

}

void
writeRegister( int Channel, uint8_t reg, uint8_t val )
{
    unsigned char data[2];

    data[0] = reg | 0x80;
    data[1] = val;
    wiringPiSPIDataRW( Channel, data, 2 );
}

uint8_t
readRegister( int Channel, uint8_t reg )
{
    unsigned char data[2];
    uint8_t val;

    data[0] = reg & 0x7F;
    data[1] = 0;
    wiringPiSPIDataRW( Channel, data, 2 );
    val = data[1];

    return val;
}

void
LogPacket( int Channel, int8_t SNR, int RSSI, double FreqError, int Bytes,
           unsigned char MessageType )
{
    if ( Config.EnablePacketLogging )
    {
        FILE *fp;

        if ( ( fp = fopen( "packets.txt", "at" ) ) != NULL )
        {
            time_t now;
            struct tm *tm;

            now = time( 0 );
            tm = localtime( &now );

            fprintf( fp,
                     "%02d:%02d:%02d - Ch %d, SNR %d, RSSI %d, FreqErr %.1lf, Bytes %d, Type %02Xh\n",
                     tm->tm_hour, tm->tm_min, tm->tm_sec, Channel, SNR, RSSI,
                     FreqError, Bytes, MessageType );

            fclose( fp );
        }
    }
}

void
LogTelemetryPacket( char *Telemetry )
{
    // if (Config.EnableTelemetryLogging)
    {
        FILE *fp;

        if ( ( fp = fopen( "telemetry.txt", "at" ) ) != NULL )
        {
            time_t now;
            struct tm *tm;

            now = time( 0 );
            tm = localtime( &now );

            fprintf( fp, "%02d:%02d:%02d - %s\n", tm->tm_hour, tm->tm_min,
                     tm->tm_sec, Telemetry );

            fclose( fp );
        }
    }
}

void
LogMessage( const char *format, ... )
{
    static WINDOW *Window = NULL;
    char Buffer[512];

    pthread_mutex_lock( &var ); // lock the critical section

    if ( Window == NULL )
    {
        // Window = newwin(25, 30, 0, 50);
        Window = newwin( LINES - 16, COLS, 16, 0 );
        scrollok( Window, TRUE );
    }

    va_list args;
    va_start( args, format );

    vsprintf( Buffer, format, args );

    va_end( args );

    if ( strlen( Buffer ) > COLS - 1 )
    {
        Buffer[COLS - 3] = '.';
        Buffer[COLS - 2] = '.';
        Buffer[COLS - 1] = '\n';
        Buffer[COLS] = 0;
    }

    waddstr( Window, Buffer );

    wrefresh( Window );

    pthread_mutex_unlock( &var );   // unlock once you are done

}

void
ChannelPrintf( int Channel, int row, int column, const char *format, ... )
{
    char Buffer[80];
    va_list args;

    pthread_mutex_lock( &var ); // lock the critical section

    va_start( args, format );

    vsprintf( Buffer, format, args );

    va_end( args );

    mvwaddstr( Config.LoRaDevices[Channel].Window, row, column, Buffer );

    wrefresh( Config.LoRaDevices[Channel].Window );

    pthread_mutex_unlock( &var );   // unlock once you are done

}

void
setMode( int Channel, uint8_t newMode )
{
    if ( newMode == currentMode )
        return;

    switch ( newMode )
    {
        case RF98_MODE_TX:
            writeRegister( Channel, REG_LNA, LNA_OFF_GAIN );    // TURN LNA OFF FOR TRANSMITT
            writeRegister( Channel, REG_PA_CONFIG, Config.LoRaDevices[Channel].Power ); // PA_MAX_UK
            writeRegister( Channel, REG_OPMODE, newMode );
            currentMode = newMode;
            break;
        case RF98_MODE_RX_CONTINUOUS:
            writeRegister( Channel, REG_PA_CONFIG, PA_OFF_BOOST );  // TURN PA OFF FOR RECIEVE??
            writeRegister( Channel, REG_LNA, LNA_MAX_GAIN );    // MAX GAIN FOR RECEIVE
            writeRegister( Channel, REG_OPMODE, newMode );
            currentMode = newMode;
            // LogMessage("Changing to Receive Continuous Mode\n");
            break;
        case RF98_MODE_SLEEP:
            writeRegister( Channel, REG_OPMODE, newMode );
            currentMode = newMode;
            // LogMessage("Changing to Sleep Mode\n"); 
            break;
        case RF98_MODE_STANDBY:
            writeRegister( Channel, REG_OPMODE, newMode );
            currentMode = newMode;
            // LogMessage("Changing to Standby Mode\n");
            break;
        default:
            return;
    }

    if ( newMode != RF98_MODE_SLEEP )
    {
        while ( digitalRead( Config.LoRaDevices[Channel].DIO5 ) == 0 )
        {
        }
        // delay(1);
    }

    // LogMessage("Mode Change Done\n");
    return;
}

void
setFrequency( int Channel, double Frequency )
{
    unsigned long FrequencyValue;
    char FrequencyString[10];

    // Format frequency as xxx.xxx.x Mhz
    sprintf( FrequencyString, "%8.4lf ", Frequency );
    FrequencyString[8] = FrequencyString[7];
    FrequencyString[7] = '.';

    FrequencyValue = ( unsigned long ) ( Frequency * 7110656 / 434 );

    writeRegister( Channel, 0x06, ( FrequencyValue >> 16 ) & 0xFF );    // Set frequency
    writeRegister( Channel, 0x07, ( FrequencyValue >> 8 ) & 0xFF );
    writeRegister( Channel, 0x08, FrequencyValue & 0xFF );

    Config.LoRaDevices[Channel].activeFreq = Frequency;

    // LogMessage("Set Frequency to %lf\n", Frequency);

    ChannelPrintf( Channel, 1, 1, "Channel %d %s MHz ", Channel,
                   FrequencyString );
}

void
setLoRaMode( int Channel )
{
    double Frequency;

    // LogMessage("Setting LoRa Mode\n");
    setMode( Channel, RF98_MODE_SLEEP );
    writeRegister( Channel, REG_OPMODE, 0x80 );

    setMode( Channel, RF98_MODE_SLEEP );

    if ( sscanf( Config.LoRaDevices[Channel].Frequency, "%lf", &Frequency ) )
    {
        // LogMessage("Set Default Frequency\n");
        setFrequency( Channel, Frequency );
    }
}

char *
BandwidthString( int Bandwidth )
{
    if ( Bandwidth == BANDWIDTH_7K8 )
        return "7.8k";
    if ( Bandwidth == BANDWIDTH_10K4 )
        return "10.4k";
    if ( Bandwidth == BANDWIDTH_15K6 )
        return "15.6k";
    if ( Bandwidth == BANDWIDTH_20K8 )
        return "20.8k";
    if ( Bandwidth == BANDWIDTH_31K25 )
        return "31.25k";
    if ( Bandwidth == BANDWIDTH_41K7 )
        return "41.7k";
    if ( Bandwidth == BANDWIDTH_62K5 )
        return "62.5k";
    if ( Bandwidth == BANDWIDTH_125K )
        return "125k";
    if ( Bandwidth == BANDWIDTH_250K )
        return "250k";
    if ( Bandwidth == BANDWIDTH_500K )
        return "500k";
    return "??k";
}

void
SetLoRaParameters( int Channel, int ImplicitOrExplicit, int ErrorCoding,
                   int Bandwidth, int SpreadingFactor,
                   int LowDataRateOptimize )
{
    writeRegister( Channel, REG_MODEM_CONFIG,
                   ImplicitOrExplicit | ErrorCoding | Bandwidth );
    writeRegister( Channel, REG_MODEM_CONFIG2, SpreadingFactor | CRC_ON );
    writeRegister( Channel, REG_MODEM_CONFIG3, 0x04 | LowDataRateOptimize );    // 0x04: AGC sets LNA gain
    writeRegister( Channel, REG_DETECT_OPT, ( readRegister( Channel, REG_DETECT_OPT ) & 0xF8 ) | ( ( SpreadingFactor == SPREADING_6 ) ? 0x05 : 0x03 ) );    // 0x05 For SF6; 0x03 otherwise
    writeRegister( Channel, REG_DETECTION_THRESHOLD, ( SpreadingFactor == SPREADING_6 ) ? 0x0C : 0x0A );    // 0x0C for SF6, 0x0A otherwise

    Config.LoRaDevices[Channel].CurrentBandwidth = Bandwidth;

    ChannelPrintf( Channel, 2, 1, "%s, %s, SF%d, EC4:%d %s",
                   ImplicitOrExplicit ==
                   IMPLICIT_MODE ? "Implicit" : "Explicit",
                   BandwidthString( Bandwidth ), SpreadingFactor >> 4,
                   ( ErrorCoding >> 1 ) + 4,
                   LowDataRateOptimize ? "LDRO" : "" );
}

void
SetDefaultLoRaParameters( int Channel )
{
    // LogMessage("Set Default Parameters\n");

    SetLoRaParameters( Channel,
                       Config.LoRaDevices[Channel].ImplicitOrExplicit,
                       Config.LoRaDevices[Channel].ErrorCoding,
                       Config.LoRaDevices[Channel].Bandwidth,
                       Config.LoRaDevices[Channel].SpreadingFactor,
                       Config.LoRaDevices[Channel].LowDataRateOptimize );
}

/////////////////////////////////////
//    Method:   Setup to receive continuously
//////////////////////////////////////
void
startReceiving( int Channel )
{
    writeRegister( Channel, REG_DIO_MAPPING_1, 0x00 );  // 00 00 00 00 maps DIO0 to RxDone

    writeRegister( Channel, REG_PAYLOAD_LENGTH, 255 );
    writeRegister( Channel, REG_RX_NB_BYTES, 255 );

    writeRegister( Channel, REG_FIFO_RX_BASE_AD, 0 );
    writeRegister( Channel, REG_FIFO_ADDR_PTR, 0 );

    // Setup Receive Continous Mode
    setMode( Channel, RF98_MODE_RX_CONTINUOUS );
}

void ReTune( int Channel, double FreqShift )
{
    setMode( Channel, RF98_MODE_SLEEP );
    LogMessage( "Retune by %lf kHz\n", FreqShift * 1000 );
    setFrequency( Channel, Config.LoRaDevices[Channel].activeFreq + FreqShift );
    startReceiving( Channel );
}

void SendLoRaData(int Channel, char *buffer, int Length)
{
    unsigned char data[257];
    int i;

	// Change frequency for the uplink ?
	if (Config.LoRaDevices[Channel].UplinkFrequency > 0)
	{
		LogMessage("Change frequency to %.3lfMHz\n", Config.LoRaDevices[Channel].UplinkFrequency);
        setFrequency(Channel, Config.LoRaDevices[Channel].UplinkFrequency);
	}
	
	// Change mode for the uplink ?
	if (Config.LoRaDevices[Channel].UplinkMode >= 0)
	{
		int UplinkMode;
		
		UplinkMode = Config.LoRaDevices[Channel].UplinkMode;
		
		LogMessage("Change LoRa mode to %d\n", Config.LoRaDevices[Channel].UplinkMode);
		
        SetLoRaParameters(Channel,
						  LoRaModes[UplinkMode].ImplicitOrExplicit,
						  LoRaModes[UplinkMode].ErrorCoding,
						  LoRaModes[UplinkMode].Bandwidth,
						  LoRaModes[UplinkMode].SpreadingFactor,
						  0);
	}
	
    LogMessage( "LoRa Channel %d Sending %d bytes\n", Channel, Length );
    Config.LoRaDevices[Channel].Sending = 1;

    setMode( Channel, RF98_MODE_STANDBY );

    writeRegister( Channel, REG_DIO_MAPPING_1, 0x40 );  // 01 00 00 00 maps DIO0 to TxDone

    writeRegister( Channel, REG_FIFO_TX_BASE_AD, 0x00 );    // Update the address ptr to the current tx base address
    writeRegister( Channel, REG_FIFO_ADDR_PTR, 0x00 );

    data[0] = REG_FIFO | 0x80;
    for ( i = 0; i < Length; i++ )
    {
        data[i + 1] = buffer[i];
    }
    wiringPiSPIDataRW( Channel, data, Length + 1 );

    // Set the length. For implicit mode, since the length needs to match what the receiver expects, we have to set a value which is 255 for an SSDV packet
    writeRegister( Channel, REG_PAYLOAD_LENGTH, Length );

    // go into transmit mode
    setMode( Channel, RF98_MODE_TX );
}

void
ShowPacketCounts( int Channel )
{
    if ( Config.LoRaDevices[Channel].InUse )
    {
        ChannelPrintf( Channel, 7, 1, "Telem Packets = %d (%us)     ",
                       Config.LoRaDevices[Channel].TelemetryCount,
                       Config.LoRaDevices[Channel].
                       LastTelemetryPacketAt ? ( unsigned int ) ( time( NULL )
                                                                  -
                                                                  Config.
                                                                  LoRaDevices
                                                                  [Channel].
                                                                  LastTelemetryPacketAt )
                       : 0 );
        ChannelPrintf( Channel, 8, 1, "Image Packets = %d (%us)     ",
                       Config.LoRaDevices[Channel].SSDVCount,
                       Config.LoRaDevices[Channel].
                       LastSSDVPacketAt ? ( unsigned int ) ( time( NULL ) -
                                                             Config.
                                                             LoRaDevices
                                                             [Channel].
                                                             LastSSDVPacketAt )
                       : 0 );

        ChannelPrintf( Channel, 9, 1, "Bad CRC = %d Bad Type = %d",
                       Config.LoRaDevices[Channel].BadCRCCount,
                       Config.LoRaDevices[Channel].UnknownCount );

        ChannelPrintf( Channel, 6, 16, "SSDV %d ",
                       Config.LoRaDevices[Channel].SSDVCount );
    }
}

void
ProcessUploadMessage( int Channel, char *Message )
{
    // LogMessage("Ch %d: Uploaded message %s\n", Channel, Message);
}

void
ProcessCallingMessage( int Channel, char *Message )
{
    char Payload[16];
    double Frequency;
    int ImplicitOrExplicit, ErrorCoding, Bandwidth, SpreadingFactor,
        LowDataRateOptimize;

    ChannelPrintf( Channel, 3, 1, "Calling message %d bytes ",
                   strlen( Message ) );

    if ( sscanf( Message + 2, "%15[^,],%lf,%d,%d,%d,%d,%d",
                 Payload,
                 &Frequency,
                 &ImplicitOrExplicit,
                 &ErrorCoding,
                 &Bandwidth, &SpreadingFactor, &LowDataRateOptimize ) == 7 )
    {
        if ( Config.LoRaDevices[Channel].AFC )
        {
            double MasterFrequency;

            sscanf( Config.LoRaDevices[Channel].Frequency, "%lf",
                    &MasterFrequency );

            Frequency +=
                Config.LoRaDevices[Channel].activeFreq - MasterFrequency;
        }

        LogMessage( "Ch %d: Calling message, new frequency %7.3lf\n", Channel,
                    Frequency );

        // Decoded OK
        setMode( Channel, RF98_MODE_SLEEP );

        // setFrequency(Channel, Config.LoRaDevices[Channel].activeFreq + );
        setFrequency( Channel, Frequency );

        SetLoRaParameters( Channel, ImplicitOrExplicit, ErrorCoding,
                           Bandwidth, SpreadingFactor, LowDataRateOptimize );

        setMode( Channel, RF98_MODE_RX_CONTINUOUS );

        Config.LoRaDevices[Channel].InCallingMode = 1;

        // ChannelPrintf(Channel, 1, 1, "Channel %d %7.3lfMHz              ", Channel, Frequency);
    }
}

size_t
write_data( void *buffer, size_t size, size_t nmemb, void *userp )
{
    return size * nmemb;
}

void
UploadListenerTelemetry( char *callsign, float gps_lat, float gps_lon,
                         char *antenna )
{
    int time_epoch = ( int ) time( NULL );
    if ( Config.EnableHabitat )
    {
        CURL *curl;
        CURLcode res;
        char PostFields[300];
        char JsonData[200];

        /* In windows, this will init the winsock stuff */

        /* get a curl handle */
        curl = curl_easy_init(  );
        if ( curl )
        {
            // So that the response to the curl POST doesn;'t mess up my finely crafted display!
            curl_easy_setopt( curl, CURLOPT_WRITEFUNCTION, write_data );

            // Set the URL that is about to receive our POST
            curl_easy_setopt( curl, CURLOPT_URL,
                              "http://habitat.habhub.org/transition/listener_telemetry" );

            // Now specify the POST data
            sprintf( JsonData, "{\"latitude\": %f, \"longitude\": %f}",
                     gps_lat, gps_lon );
            sprintf( PostFields, "callsign=%s&time=%d&data=%s", callsign,
                     time_epoch, JsonData );
            curl_easy_setopt( curl, CURLOPT_POSTFIELDS, PostFields );

            // Perform the request, res will get the return code
            res = curl_easy_perform( curl );

            // Check for errors
            if ( res == CURLE_OK )
            {
                LogMessage( "Uploaded listener %s position %f,%f\n",
                            Config.Tracker, Config.latitude,
                            Config.longitude );
            }
            else
            {
                LogMessage( "curl_easy_perform() failed: %s\n",
                            curl_easy_strerror( res ) );
            }

            // always cleanup
            curl_easy_cleanup( curl );
        }

        /* In windows, this will init the winsock stuff */

        /* get a curl handle */
        curl = curl_easy_init(  );
        if ( curl )
        {
            // So that the response to the curl POST doesn;'t mess up my finely crafted display!
            curl_easy_setopt( curl, CURLOPT_WRITEFUNCTION, write_data );

            // Set the URL that is about to receive our POST
            curl_easy_setopt( curl, CURLOPT_URL,
                              "http://habitat.habhub.org/transition/listener_information" );

            // Now specify the POST data
            sprintf( JsonData, "{\"radio\": \"%s\", \"antenna\": \"%s\"}",
                     "LoRa RFM98W", antenna );
            sprintf( PostFields, "callsign=%s&time=%d&data=%s", Config.Tracker, time_epoch, JsonData );
            curl_easy_setopt( curl, CURLOPT_POSTFIELDS, PostFields );

            // Perform the request, res will get the return code
            res = curl_easy_perform( curl );

            // Check for errors
            if ( res != CURLE_OK )
            {
                LogMessage( "curl_easy_perform() failed: %s\n",
                            curl_easy_strerror( res ) );
            }

            // always cleanup
            curl_easy_cleanup( curl );
        }

    }
}


void
DoPositionCalcs( Channel )
{
    unsigned long Now;
    struct tm tm;
    float Climb, Period;

    strptime( Config.LoRaDevices[Channel].Time, "%H:%M:%S", &tm );
    Now = tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec;

    if ( ( Config.LoRaDevices[Channel].LastPositionAt > 0 )
         && ( Now > Config.LoRaDevices[Channel].LastPositionAt ) )
    {
        Climb =
            ( float ) Config.LoRaDevices[Channel].Altitude -
            ( float ) Config.LoRaDevices[Channel].PreviousAltitude;
        Period =
            ( float ) Now -
            ( float ) Config.LoRaDevices[Channel].LastPositionAt;
        Config.LoRaDevices[Channel].AscentRate = Climb / Period;
    }
    else
    {
        Config.LoRaDevices[Channel].AscentRate = 0;
    }

    Config.LoRaDevices[Channel].PreviousAltitude =
        Config.LoRaDevices[Channel].Altitude;
    Config.LoRaDevices[Channel].LastPositionAt = Now;

    ChannelPrintf( Channel, 4, 1, "%8.5lf, %8.5lf, %05u   ",
                   Config.LoRaDevices[Channel].Latitude,
                   Config.LoRaDevices[Channel].Longitude,
                   Config.LoRaDevices[Channel].Altitude );
}


void
ProcessLine( int Channel, char *Line )
{
    int Satellites;
    float TempInt, TempExt;

    Config.LoRaDevices[Channel].FlightMode = -1;

    if ( Config.EnableDev )
    {
        sscanf( Line + 2,
                "%15[^,],%u,%8[^,],%lf,%lf,%u,%d,%d,%d,%f,%f,%lf,%lf,%lf,%lf,%d,%d,%d,%lf,%d,%d,%d,%d,%lf,%d",
                ( Config.LoRaDevices[Channel].Payload ),
                &( Config.LoRaDevices[Channel].Counter ),
                ( Config.LoRaDevices[Channel].Time ),
                &( Config.LoRaDevices[Channel].Latitude ),
                &( Config.LoRaDevices[Channel].Longitude ),
                &( Config.LoRaDevices[Channel].Altitude ),
                &( Config.LoRaDevices[Channel].Speed ),
                &( Config.LoRaDevices[Channel].Heading ), &Satellites,
                &TempInt, &TempExt, &( Config.LoRaDevices[Channel].cda ),
                &( Config.LoRaDevices[Channel].PredictedLatitude ),
                &( Config.LoRaDevices[Channel].PredictedLongitude ),
                &( Config.LoRaDevices[Channel].PredictedLandingSpeed ),
                &( Config.LoRaDevices[Channel].PredictedTime ),
                &( Config.LoRaDevices[Channel].CompassActual ),
                &( Config.LoRaDevices[Channel].CompassTarget ),
                &( Config.LoRaDevices[Channel].AirSpeed ),
                &( Config.LoRaDevices[Channel].AirDirection ),
                &( Config.LoRaDevices[Channel].ServoLeft ),
                &( Config.LoRaDevices[Channel].ServoRight ),
                &( Config.LoRaDevices[Channel].ServoTime ),
                &( Config.LoRaDevices[Channel].GlideRatio ),
                &( Config.LoRaDevices[Channel].FlightMode ) );
    }
    else
    {
        sscanf( Line + 2, "%15[^,],%u,%8[^,],%lf,%lf,%u",
                ( Config.LoRaDevices[Channel].Payload ),
                &( Config.LoRaDevices[Channel].Counter ),
                ( Config.LoRaDevices[Channel].Time ),
                &( Config.LoRaDevices[Channel].Latitude ),
                &( Config.LoRaDevices[Channel].Longitude ),
                &( Config.LoRaDevices[Channel].Altitude ) );
    }
}


void
ProcessTelemetryMessage( int Channel, char *Message )
{
    if ( strlen( Message + 1 ) < 250 )
    {
        char *startmessage, *endmessage;

        ChannelPrintf( Channel, 3, 1, "Telemetry %d bytes       ",
                       strlen( Message + 1 ) );

        startmessage = Message;
        endmessage = strchr( startmessage, '\n' );

        if ( endmessage != NULL )
        {
            habitate_telem_packets++;

            time_t now;
            struct tm *tm;

            *endmessage = '\0';

			LogTelemetryPacket(startmessage);

            strcpy( Config.LoRaDevices[Channel].Telemetry, startmessage );
            // UploadTelemetryPacket(startmessage);

            ProcessLine( Channel, startmessage );

            now = time( 0 );
            tm = localtime( &now );


            if ( Config.EnableHabitat )
            {

                // Create a telemetry packet
                telemetry_t t;
                t.Channel = Channel;
                t.Packet_Number = habitate_telem_packets;
                memcpy( t.Telemetry, startmessage,
                        strlen( startmessage ) + 1 );

                // Add the telemetry packet to the pipe
                int result = write( telem_pipe_fd[1], &t, sizeof( t ) );
                if ( result == -1 )
                {
                    printf( "Error writing to the telemetry pipe\n" );
                    exit( 1 );
                }
                if ( result == 0 )
                {
                    LogMessage( "Nothing written to telemetry pipe \n" );
                }
                if ( result > 1 )
                {
                    htsv.packet_count++;
                }
            }

            LogMessage( "%02d:%02d:%02d Ch%d: %s\n", tm->tm_hour, tm->tm_min,
                        tm->tm_sec, Channel, startmessage );

        }

        DoPositionCalcs( Channel );

        // RJH I think this should be moved up to the bottom of the loop above  
        Config.LoRaDevices[Channel].TelemetryCount++;
        Config.LoRaDevices[Channel].LastTelemetryPacketAt = time( NULL );
    }
}

static char *
decode_callsign( char *callsign, uint32_t code )
{
    char *c, s;

    *callsign = '\0';

    /* Is callsign valid? */
    if ( code > 0xF423FFFF )
        return ( callsign );

    for ( c = callsign; code; c++ )
    {
        s = code % 40;
        if ( s == 0 )
            *c = '-';
        else if ( s < 11 )
            *c = '0' + s - 1;
        else if ( s < 14 )
            *c = '-';
        else
            *c = 'A' + s - 14;
        code /= 40;
    }
    *c = '\0';

    return ( callsign );
}

int
FileExists( char *filename )
{
    struct stat st;

    return stat( filename, &st ) == 0;
}

void
ProcessSSDVMessage( int Channel, char *Message )
{
    // SSDV packet
    uint32_t CallsignCode;
    char Callsign[7], *FileMode;
    int ImageNumber, PacketNumber;
    char filename[100];
    FILE *fp;

    Message[0] = 0x55;

    CallsignCode = Message[2];
    CallsignCode <<= 8;
    CallsignCode |= Message[3];
    CallsignCode <<= 8;
    CallsignCode |= Message[4];
    CallsignCode <<= 8;
    CallsignCode |= Message[5];

    decode_callsign( Callsign, CallsignCode );

    ImageNumber = Message[6];
    PacketNumber = Message[7] * 256 + Message[8];

    LogMessage( "Ch%d: SSDV Packet, Callsign %s, Image %d, Packet %d\n",
                Channel, Callsign, Message[6], PacketNumber );
    ChannelPrintf( Channel, 3, 1, "SSDV Packet                     " );
    ChannelPrintf( Channel, 5, 1, "SSDV %s: Image %d, Packet %d", Callsign,
                   Message[6], PacketNumber );

    // Create new file ?
    sprintf( filename, "/tmp/%s_%d.bin", Callsign, ImageNumber );
    if ( !FileExists( filename ) )
    {
        // New image so new file
        LogMessage( "Started image %d\n", ImageNumber );

        // AddImageNumberToLog(Channel, ImageNumber);

        FileMode = "wb";
    }
    else
    {
        FileMode = "r+b";
    }

    // Save to file
    if ( ( fp = fopen( filename, FileMode ) ) )
    {
        fseek( fp, PacketNumber * 256, SEEK_SET );
        if ( fwrite( Message, 1, 256, fp ) == 256 )
        {
            // AddImagePacketToLog(Channel, ImageNumber, PacketNumber);
        }
        else
        {
            LogMessage( "** FAILED TO WRITE TO SSDV FILE\n" );
        }

        fclose( fp );
    }

    // ShowMissingPackets(Channel);

    if ( Config.EnableSSDV )
    {
        // Create a SSDV packet
        ssdv_t s;
        s.Channel = Channel;
        s.Packet_Number = Config.LoRaDevices[Channel].SSDVCount;
        memcpy( s.SSDV_Packet, Message, 256 );

        // Add the SSDV packet to the pipe
        int result = write( ssdv_pipe_fd[1], &s, sizeof( s ) );
        if ( result == -1 )
        {
            printf( "Error writing to the issdv pipe\n" );
            exit( 1 );
        }
        if ( result == 0 )
        {
            LogMessage( "Nothing written to ssdv pipe \n" );
        }
        if ( result > 1 )
        {
            stsv.packet_count++;
        }

    }

    Config.LoRaDevices[Channel].SSDVCount++;
    Config.LoRaDevices[Channel].LastSSDVPacketAt = time( NULL );
}

void
TestMessageForSMSAcknowledgement( int Channel, char *Message )
{
    if ( Config.SMSFolder[0] )
    {
        if ( strlen( Message ) < 250 )
        {
            char Line[256];
            char *token, *value1, *value2;
            int FileNumber;

            value1 = NULL;
            value2 = NULL;

            strcpy( Line, Message );
            token = strtok( Line, "," );
            while ( token != NULL )
            {
                value1 = value2;
                value2 = token;
                token = strtok( NULL, "," );
            }

            // Rename the file matching this parameter
            if ( value1 )
            {
                char OldFileName[256], NewFileName[256];
                FileNumber = atoi( value1 );
                if ( FileNumber > 0 )
                {
                    sprintf( OldFileName, "%s/%d.sms", Config.SMSFolder, FileNumber );
                    if ( FileExists( OldFileName ) )
                    {
                        sprintf( NewFileName, "%s/%d.ack", Config.SMSFolder, FileNumber );
                        if ( FileExists( NewFileName ) )
                        {
                            remove( NewFileName );
                        }
                        rename( OldFileName, NewFileName );
                        LogMessage( "Renamed %s as %s\n", OldFileName,
                                    NewFileName );
                    }
                }
            }
        }
    }
}

void
DIO0_Interrupt( int Channel )
{
    if ( Config.LoRaDevices[Channel].Sending )
    {
        Config.LoRaDevices[Channel].Sending = 0;
        LogMessage( "Ch%d: End of Tx\n", Channel );

        setLoRaMode( Channel );
        SetDefaultLoRaParameters( Channel );
        startReceiving( Channel );
    }
    else
    {
        int Bytes;
        char Message[257];

        Bytes = receiveMessage( Channel, Message + 1 );

        // hexdump_buffer ("Raw Data", Message, 257);


        if ( Bytes > 0 )
        {
            if ( Config.LoRaDevices[Channel].ActivityLED >= 0 )
            {
                digitalWrite( Config.LoRaDevices[Channel].ActivityLED, 1 );
                LEDCounts[Channel] = 5;
            }

            if ( Message[1] == '!' )
            {
                ProcessUploadMessage( Channel, Message + 1 );
            }
            else if ( Message[1] == '^' )
            {
                ProcessCallingMessage( Channel, Message + 1 );
            }
            else if ( Message[1] == '$' )
            {
                ProcessTelemetryMessage( Channel, Message + 1 );
                TestMessageForSMSAcknowledgement( Channel, Message + 1 );
            }
            else if ( Message[1] == '>' )
            {
                LogMessage( "Flight Controller message %d bytes = %s", Bytes,
                            Message + 1 );
            }
            else if ( Message[1] == '*' )
            {
                LogMessage( "Uplink Command message %d bytes = %s", Bytes,
                            Message + 1 );
            }
            else if ((Message[1] == 0x66) || (Message[1] == 0x67) || (Message[1] == 0x68) || (Message[1] == 0x69))
            {
                ProcessSSDVMessage( Channel, Message );
            }
            else
            {
                LogMessage( "Unknown packet type is %02Xh, RSSI %d\n",
                            Message[1], readRegister( Channel,
                                                      REG_PACKET_RSSI ) -
                            157 );
                ChannelPrintf( Channel, 3, 1, "Unknown Packet %d, %d bytes",
                               Message[0], Bytes );
                Config.LoRaDevices[Channel].UnknownCount++;
            }

            Config.LoRaDevices[Channel].LastPacketAt = time( NULL );

            if ( Config.LoRaDevices[Channel].InCallingMode
                 && ( Config.CallingTimeout > 0 ) )
            {
                Config.LoRaDevices[Channel].ReturnToCallingModeAt =
                    time( NULL ) + Config.CallingTimeout;
            }

            ShowPacketCounts( Channel );
        }
    }
}

void DIO_Ignore_Interrupt_0( void )
{
    // nothing, obviously!
}

void
DIO0_Interrupt_0( void )
{
    DIO0_Interrupt( 0 );
}

void
DIO0_Interrupt_1( void )
{
    DIO0_Interrupt( 1 );
}

void
setupRFM98( int Channel )
{
    if ( Config.LoRaDevices[Channel].InUse )
    {
        // initialize the pins
        pinMode( Config.LoRaDevices[Channel].DIO0, INPUT );
        pinMode( Config.LoRaDevices[Channel].DIO5, INPUT );

        wiringPiISR( Config.LoRaDevices[Channel].DIO0, INT_EDGE_RISING,
                     Channel > 0 ? &DIO0_Interrupt_1 : &DIO0_Interrupt_0 );

        if ( wiringPiSPISetup( Channel, 500000 ) < 0 )
        {
            fprintf( stderr,
                     "Failed to open SPI port.  Try loading spi library with 'gpio load spi'" );
            exit( 1 );
        }

        // LoRa mode 
        setLoRaMode( Channel );

        SetDefaultLoRaParameters( Channel );

        startReceiving( Channel );
    }
}

double
FrequencyReference( int Channel )
{
    switch ( Config.LoRaDevices[Channel].CurrentBandwidth )
    {
        case BANDWIDTH_7K8:
            return 7800;
        case BANDWIDTH_10K4:
            return 10400;
        case BANDWIDTH_15K6:
            return 15600;
        case BANDWIDTH_20K8:
            return 20800;
        case BANDWIDTH_31K25:
            return 31250;
        case BANDWIDTH_41K7:
            return 41700;
        case BANDWIDTH_62K5:
            return 62500;
        case BANDWIDTH_125K:
            return 125000;
        case BANDWIDTH_250K:
            return 250000;
        case BANDWIDTH_500K:
            return 500000;
    }

    return 0;
}

double
FrequencyError( int Channel )
{
    int32_t Temp;

    Temp = ( int32_t ) readRegister( Channel, REG_FREQ_ERROR ) & 7;
    Temp <<= 8L;
    Temp += ( int32_t ) readRegister( Channel, REG_FREQ_ERROR + 1 );
    Temp <<= 8L;
    Temp += ( int32_t ) readRegister( Channel, REG_FREQ_ERROR + 2 );

    if ( readRegister( Channel, REG_FREQ_ERROR ) & 8 )
    {
        Temp = Temp - 524288;
    }

    return -( ( double ) Temp * ( 1 << 24 ) / 32000000.0 ) *
        ( FrequencyReference( Channel ) / 500000.0 );
}

int
receiveMessage( int Channel, char *message )
{
    int i, Bytes, currentAddr, x;
    unsigned char data[257];
    double FreqError;

    Bytes = 0;

    x = readRegister( Channel, REG_IRQ_FLAGS );
    // LogMessage("Message status = %02Xh\n", x);

    // clear the rxDone flag
    writeRegister( Channel, REG_IRQ_FLAGS, 0x40 );

    // check for payload crc issues (0x20 is the bit we are looking for
    if ( ( x & 0x20 ) == 0x20 )
    {
        LogMessage( "Ch%d: CRC Failure, RSSI %d\n", Channel,
                    readRegister( Channel, REG_PACKET_RSSI ) - 157 );
        // reset the crc flags
        writeRegister( Channel, REG_IRQ_FLAGS, 0x20 );
        ChannelPrintf( Channel, 3, 1, "CRC Failure %02Xh!!\n", x );
        Config.LoRaDevices[Channel].BadCRCCount++;
        ShowPacketCounts( Channel );
    }
    else
    {
        int8_t SNR;
        int RSSI;

        currentAddr = readRegister( Channel, REG_FIFO_RX_CURRENT_ADDR );
        Bytes = readRegister( Channel, REG_RX_NB_BYTES );

        SNR = readRegister( Channel, REG_PACKET_SNR );
        SNR /= 4;
        RSSI = readRegister( Channel, REG_PACKET_RSSI ) - 157;
        if ( SNR < 0 )
        {
            RSSI += SNR;
        }

        ChannelPrintf( Channel, 10, 1, "Packet SNR = %d, RSSI = %d      ",
                       ( int ) SNR, RSSI );

        FreqError = FrequencyError( Channel ) / 1000;
        ChannelPrintf( Channel, 11, 1, "Freq. Error = %5.1lfkHz ",
                       FreqError );


        writeRegister( Channel, REG_FIFO_ADDR_PTR, currentAddr );

        data[0] = REG_FIFO;
        wiringPiSPIDataRW( Channel, data, Bytes + 1 );
        for ( i = 0; i <= Bytes; i++ )
        {
            message[i] = data[i + 1];
        }

        message[Bytes] = '\0';

        LogPacket( Channel, SNR, RSSI, FreqError, Bytes, message[1] );

        if ( Config.LoRaDevices[Channel].AFC && ( fabs( FreqError ) > 0.5 ) )
        {
            ReTune( Channel, FreqError / 1000 );
        }
    }

    // Clear all flags
    writeRegister( Channel, REG_IRQ_FLAGS, 0xFF );

    return Bytes;
}

void ReadString( FILE * fp, char *keyword, char *Result, int Length, int NeedValue )
{
    char line[100], *token, *value;

    fseek( fp, 0, SEEK_SET );
    *Result = '\0';

    while ( fgets( line, sizeof( line ), fp ) != NULL )
    {
		line[strcspn(line, "\r")] = '\0';		// Get rid of CR if there is one
		
        token = strtok( line, "=" );
        if ( strcasecmp( keyword, token ) == 0 )
        {
            value = strtok( NULL, "\n" );
            strcpy( Result, value );
            return;
        }
    }

    if ( NeedValue )
    {
        LogMessage( "Missing value for '%s' in configuration file\n", keyword );
        exit( 1 );
    }
}

int
ReadInteger( FILE * fp, char *keyword, int NeedValue, int DefaultValue )
{
    char Temp[64];

    ReadString( fp, keyword, Temp, sizeof( Temp ), NeedValue );

    if ( Temp[0] )
    {
        return atoi( Temp );
    }

    return DefaultValue;
}

float
ReadFloat( FILE * fp, char *keyword )
{
    char Temp[64];

    ReadString( fp, keyword, Temp, sizeof( Temp ), 0 );

    if ( Temp[0] )
    {
        return atof( Temp );
    }

    return 0;
}

int
ReadBoolean( FILE * fp, char *keyword, int NeedValue, int *Result )
{
    char Temp[32];

    ReadString( fp, keyword, Temp, sizeof( Temp ), NeedValue );

    if ( *Temp )
    {
        *Result = ( *Temp == '1' ) || ( *Temp == 'Y' ) || ( *Temp == 'y' )
            || ( *Temp == 't' ) || ( *Temp == 'T' );
    }

    return *Temp;
}

void RemoveTrailingSlash(char *Value)
{
	int Len;
	
	if ((Len = strlen(Value)) > 0)
	{
		if ((Value[Len-1] == '/') || (Value[Len-1] == '\\'))
		{
			Value[Len-1] = '\0';
		}
	}
}

void LoadConfigFile(void)
{
    FILE *fp;
    char *filename = "gateway.txt";
    char Keyword[32];
    int Channel, Temp;
    char TempString[16];

    Config.LoRaDevices[0].InUse = 0;
    Config.LoRaDevices[1].InUse = 0;
    Config.EnableHabitat = 1;
    Config.EnableSSDV = 1;
    Config.EnableTelemetryLogging = 0;
    Config.EnablePacketLogging = 0;
    Config.SSDVJpegFolder[0] = '\0';
    Config.ftpServer[0] = '\0';
    Config.ftpUser[0] = '\0';
    Config.ftpPassword[0] = '\0';
    Config.ftpFolder[0] = '\0';
    Config.latitude = -999;
    Config.longitude = -999;
    Config.antenna[0] = '\0';
    Config.EnableDev = 0;

    // Default pin allocations
    Config.LoRaDevices[0].DIO0 = 6;
    Config.LoRaDevices[0].DIO5 = 5;

    Config.LoRaDevices[1].DIO0 = 27;
    Config.LoRaDevices[1].DIO5 = 26;

    if ( ( fp = fopen( filename, "r" ) ) == NULL )
    {
        printf
            ( "\nFailed to open config file %s (error %d - %s).\nPlease check that it exists and has read permission.\n",
              filename, errno, strerror( errno ) );
        exit( 1 );
    }

    // Receiver config
    ReadString( fp, "tracker", Config.Tracker, sizeof( Config.Tracker ), 1 );
    LogMessage( "Tracker = '%s'\n", Config.Tracker );

    // Enable uploads
    ReadBoolean( fp, "EnableHabitat", 0, &Config.EnableHabitat );
    ReadBoolean( fp, "EnableSSDV", 0, &Config.EnableSSDV );

    // Enable telemetry logging
    ReadBoolean( fp, "LogTelemetry", 0, &Config.EnableTelemetryLogging );

    // Enable packet logging
    ReadBoolean( fp, "LogPackets", 0, &Config.EnablePacketLogging );

    // Calling mode
    Config.CallingTimeout = ReadInteger( fp, "CallingTimeout", 0, 300 );

    // LED allocations
    Config.NetworkLED = ReadInteger( fp, "NetworkLED", 0, -1 );
    Config.InternetLED = ReadInteger( fp, "InternetLED", 0, -1 );
    Config.LoRaDevices[0].ActivityLED =
        ReadInteger( fp, "ActivityLED_0", 0, -1 );
    Config.LoRaDevices[1].ActivityLED =
        ReadInteger( fp, "ActivityLED_1", 0, -1 );

    // Server Port
    Config.ServerPort = ReadInteger( fp, "ServerPort", 0, -1 );

    // SSDV Settings
    ReadString( fp, "jpgFolder", Config.SSDVJpegFolder, sizeof( Config.SSDVJpegFolder ), 0 );
    if ( Config.SSDVJpegFolder[0] )
    {
        // Create SSDV Folders
        struct stat st = { 0 };

        if ( stat( Config.SSDVJpegFolder, &st ) == -1 )
        {
            mkdir( Config.SSDVJpegFolder, 0777 );
        }
    }

    // ftp images
    ReadString( fp, "ftpserver", Config.ftpServer, sizeof( Config.ftpServer ),
                0 );
    ReadString( fp, "ftpUser", Config.ftpUser, sizeof( Config.ftpUser ), 0 );
    ReadString( fp, "ftpPassword", Config.ftpPassword,
                sizeof( Config.ftpPassword ), 0 );
    ReadString( fp, "ftpFolder", Config.ftpFolder, sizeof( Config.ftpFolder ),
                0 );

    // Listener
    Config.latitude = ReadFloat( fp, "Latitude" );
    Config.longitude = ReadFloat( fp, "Longitude" );
    ReadString( fp, "antenna", Config.antenna, sizeof( Config.antenna ), 0 );

    // Dev mode
    ReadBoolean( fp, "EnableDev", 0, &Config.EnableDev );

    // SMS upload to tracker
    Config.SMSFolder[0] = '\0';
    ReadString(fp, "SMSFolder", Config.SMSFolder, sizeof( Config.SMSFolder ), 0);
    if ( Config.SMSFolder[0] )
    {
		RemoveTrailingSlash(Config.SMSFolder);
        LogMessage( "Folder %s will be scanned for messages to upload\n", Config.SMSFolder );
    }

    for ( Channel = 0; Channel <= 1; Channel++ )
    {
        // Defaults
        Config.LoRaDevices[Channel].Frequency[0] = '\0';

        sprintf( Keyword, "frequency_%d", Channel );
        ReadString( fp, Keyword, Config.LoRaDevices[Channel].Frequency,
                    sizeof( Config.LoRaDevices[Channel].Frequency ), 0 );
        if ( Config.LoRaDevices[Channel].Frequency[0] )
        {
            Config.LoRaDevices[Channel].ImplicitOrExplicit = EXPLICIT_MODE;
            Config.LoRaDevices[Channel].ErrorCoding = ERROR_CODING_4_8;
            Config.LoRaDevices[Channel].Bandwidth = BANDWIDTH_20K8;
            Config.LoRaDevices[Channel].SpreadingFactor = SPREADING_11;
            Config.LoRaDevices[Channel].LowDataRateOptimize = 0x00;
            Config.LoRaDevices[Channel].AFC = FALSE;

            LogMessage( "Channel %d frequency set to %s\n", Channel,
                        Config.LoRaDevices[Channel].Frequency );
            Config.LoRaDevices[Channel].InUse = 1;

            // DIO0 / DIO5 overrides
            sprintf( Keyword, "DIO0_%d", Channel );
            Config.LoRaDevices[Channel].DIO0 =
                ReadInteger( fp, Keyword, 0,
                             Config.LoRaDevices[Channel].DIO0 );

            sprintf( Keyword, "DIO5_%d", Channel );
            Config.LoRaDevices[Channel].DIO5 =
                ReadInteger( fp, Keyword, 0,
                             Config.LoRaDevices[Channel].DIO5 );

            LogMessage( "LoRa Channel %d DIO0=%d DIO5=%d\n", Channel,
                        Config.LoRaDevices[Channel].DIO0,
                        Config.LoRaDevices[Channel].DIO5 );

            // Uplink
            sprintf( Keyword, "UplinkTime_%d", Channel );
            Config.LoRaDevices[Channel].UplinkTime = ReadInteger( fp, Keyword, 0, 0 );
            sprintf( Keyword, "UplinkCycle_%d", Channel );
            Config.LoRaDevices[Channel].UplinkCycle = ReadInteger( fp, Keyword, 0, 0 );
            LogMessage( "Channel %d UplinkTime %d Uplink Cycle %d\n", Channel,
                        Config.LoRaDevices[Channel].UplinkTime,
                        Config.LoRaDevices[Channel].UplinkCycle );

            sprintf( Keyword, "Power_%d", Channel );
            Config.LoRaDevices[Channel].Power = ReadInteger( fp, Keyword, 0, PA_MAX_UK );
            LogMessage( "Channel %d power set to %02Xh\n", Channel, Config.LoRaDevices[Channel].Power );

            sprintf( Keyword, "UplinkMode_%d", Channel );
            Config.LoRaDevices[Channel].UplinkMode = ReadInteger( fp, Keyword, 0, -1);
			if (Config.LoRaDevices[Channel].UplinkMode >= 0)
			{
				LogMessage( "Channel %d uplink mode %d\n", Channel, Config.LoRaDevices[Channel].UplinkMode);
			}

            sprintf( Keyword, "UplinkFrequency_%d", Channel );
			Config.LoRaDevices[Channel].UplinkFrequency = 0;
            Config.LoRaDevices[Channel].UplinkFrequency = ReadFloat(fp, Keyword);
			if (Config.LoRaDevices[Channel].UplinkFrequency > 0)
            {
				LogMessage( "Channel %d uplink frequency %.3lfMHz\n", Channel, Config.LoRaDevices[Channel].UplinkFrequency);
			}

            Config.LoRaDevices[Channel].SpeedMode = 0;

            sprintf( Keyword, "mode_%d", Channel );
            Config.LoRaDevices[Channel].SpeedMode = ReadInteger( fp, Keyword, 0, 0 );

			if ((Config.LoRaDevices[Channel].SpeedMode < 0) || (Config.LoRaDevices[Channel].SpeedMode >= sizeof(LoRaModes)/sizeof(LoRaModes[0]))) Config.LoRaDevices[Channel].SpeedMode = 0;

			Config.LoRaDevices[Channel].ImplicitOrExplicit = LoRaModes[Config.LoRaDevices[Channel].SpeedMode].ImplicitOrExplicit;
			Config.LoRaDevices[Channel].ErrorCoding = LoRaModes[Config.LoRaDevices[Channel].SpeedMode].ErrorCoding;
			Config.LoRaDevices[Channel].Bandwidth = LoRaModes[Config.LoRaDevices[Channel].SpeedMode].Bandwidth;
			Config.LoRaDevices[Channel].SpreadingFactor = LoRaModes[Config.LoRaDevices[Channel].SpeedMode].SpreadingFactor;
			Config.LoRaDevices[Channel].LowDataRateOptimize = LoRaModes[Config.LoRaDevices[Channel].SpeedMode].LowDataRateOptimize;

            sprintf( Keyword, "sf_%d", Channel );
            Temp = ReadInteger( fp, Keyword, 0, 0 );
            if ( ( Temp >= 6 ) && ( Temp <= 12 ) )
            {
                Config.LoRaDevices[Channel].SpreadingFactor = Temp << 4;
                LogMessage( "Setting SF=%d\n", Temp );
            }

            sprintf( Keyword, "bandwidth_%d", Channel );
            ReadString( fp, Keyword, TempString, sizeof( TempString ), 0 );
            if ( *TempString )
            {
                LogMessage( "Setting BW=%s\n", TempString );
            }
            if ( strcmp( TempString, "7K8" ) == 0 )
            {
                Config.LoRaDevices[Channel].Bandwidth = BANDWIDTH_7K8;
            }
            else if ( strcmp( TempString, "10K4" ) == 0 )
            {
                Config.LoRaDevices[Channel].Bandwidth = BANDWIDTH_10K4;
            }
            else if ( strcmp( TempString, "15K6" ) == 0 )
            {
                Config.LoRaDevices[Channel].Bandwidth = BANDWIDTH_15K6;
            }
            else if ( strcmp( TempString, "20K8" ) == 0 )
            {
                Config.LoRaDevices[Channel].Bandwidth = BANDWIDTH_20K8;
            }
            else if ( strcmp( TempString, "31K25" ) == 0 )
            {
                Config.LoRaDevices[Channel].Bandwidth = BANDWIDTH_31K25;
            }
            else if ( strcmp( TempString, "41K7" ) == 0 )
            {
                Config.LoRaDevices[Channel].Bandwidth = BANDWIDTH_41K7;
            }
            else if ( strcmp( TempString, "62K5" ) == 0 )
            {
                Config.LoRaDevices[Channel].Bandwidth = BANDWIDTH_62K5;
            }
            else if ( strcmp( TempString, "125K" ) == 0 )
            {
                Config.LoRaDevices[Channel].Bandwidth = BANDWIDTH_125K;
            }
            else if ( strcmp( TempString, "250K" ) == 0 )
            {
                Config.LoRaDevices[Channel].Bandwidth = BANDWIDTH_250K;
            }
            else if ( strcmp( TempString, "500K" ) == 0 )
            {
                Config.LoRaDevices[Channel].Bandwidth = BANDWIDTH_500K;
            }

            sprintf( Keyword, "implicit_%d", Channel );
            if ( ReadBoolean( fp, Keyword, 0, &Temp ) )
            {
                Config.LoRaDevices[Channel].ImplicitOrExplicit =
                    Temp ? IMPLICIT_MODE : EXPLICIT_MODE;
            }

            sprintf( Keyword, "coding_%d", Channel );
            Temp = ReadInteger( fp, Keyword, 0, 0 );
            if ( ( Temp >= 5 ) && ( Temp <= 8 ) )
            {
                Config.LoRaDevices[Channel].ErrorCoding = ( Temp - 4 ) << 1;
                LogMessage( "Setting Error Coding=%d\n", Temp );
            }

            sprintf( Keyword, "lowopt_%d", Channel );
            if ( ReadBoolean( fp, Keyword, 0, &Temp ) )
            {
                if ( Temp )
                {
                    Config.LoRaDevices[Channel].LowDataRateOptimize = 0x08;
                }
            }

            sprintf( Keyword, "AFC_%d", Channel );
            if ( ReadBoolean( fp, Keyword, 0, &Temp ) )
            {
                if ( Temp )
                {
                    Config.LoRaDevices[Channel].AFC = TRUE;
                    ChannelPrintf( Channel, 11, 24, "AFC" );
                }
            }

            // Clear any flags left over from a previous run
            writeRegister( Channel, REG_IRQ_FLAGS, 0xFF );
        }
    }

    fclose( fp );
}

void
LoadPayloadFile( int ID )
{
    FILE *fp;
    char filename[16];

    sprintf( filename, "payload_%d.txt", ID );

    if ( ( fp = fopen( filename, "r" ) ) != NULL )
    {
        LogMessage( "Reading payload file %s\n", filename );
        ReadString( fp, "payload", Payloads[ID].Payload,
                    sizeof( Payloads[ID].Payload ), 1 );
        LogMessage( "Payload %d = '%s'\n", ID, Payloads[ID].Payload );

        Payloads[ID].InUse = 1;

        fclose( fp );
    }
    else
    {
        strcpy( Payloads[ID].Payload, "Unknown" );
        Payloads[ID].InUse = 0;
    }
}

void
LoadPayloadFiles( void )
{
    int ID;

    for ( ID = 0; ID < 16; ID++ )
    {
        LoadPayloadFile( ID );
    }
}

WINDOW *
InitDisplay( void )
{
    WINDOW *mainwin;
    int Channel;

    /*  Initialize ncurses  */

    if ( ( mainwin = initscr(  ) ) == NULL )
    {
        fprintf( stderr, "Error initialising ncurses.\n" );
        exit( EXIT_FAILURE );
    }

    start_color(  );            /*  Initialize colours  */

    if ( COLORS == 256 )
    {
        init_pair( 1, COLOR_WHITE, 22 );
        init_pair( 2, COLOR_YELLOW, 22 );
    }
    else
    {
        init_pair( 1, COLOR_WHITE, COLOR_BLUE );
        init_pair( 2, COLOR_YELLOW, COLOR_BLUE );
    }

    color_set( 1, NULL );
    // bkgd(COLOR_PAIR(1));
    // attrset(COLOR_PAIR(1) | A_BOLD);

    char title[80];

    sprintf( title, "LoRa Habitat and SSDV Gateway by M0RPI, M0RJX - " VERSION);

    // Title bar
    mvaddstr( 0, ( 80 - strlen( title ) ) / 2, title );
    refresh(  );

    // Windows for LoRa live data
    for ( Channel = 0; Channel <= 1; Channel++ )
    {
        Config.LoRaDevices[Channel].Window =
            newwin( 14, 38, 1, Channel ? 41 : 1 );
        wbkgd( Config.LoRaDevices[Channel].Window, COLOR_PAIR( 2 ) );

        // wcolor_set(Config.LoRaDevices[Channel].Window, 2, NULL);
        // waddstr(Config.LoRaDevices[Channel].Window, "WINDOW");
        // mvwaddstr(Config.LoRaDevices[Channel].Window, 0, 0, "Window");
        wrefresh( Config.LoRaDevices[Channel].Window );
    }

    curs_set( 0 );

    return mainwin;
}

void
CloseDisplay( WINDOW * mainwin )
{
    /*  Clean up after ourselves  */
    delwin( mainwin );
    endwin(  );
    refresh(  );
}


uint16_t
CRC16( unsigned char *ptr )
{
    uint16_t CRC;
    int j;

    CRC = 0xffff;               // Seed

    for ( ; *ptr; ptr++ )
    {                           // For speed, repeat calculation instead of looping for each bit
        CRC ^= ( ( ( unsigned int ) *ptr ) << 8 );
        for ( j = 0; j < 8; j++ )
        {
            if ( CRC & 0x8000 )
                CRC = ( CRC << 1 ) ^ 0x1021;
            else
                CRC <<= 1;
        }
    }

    return CRC;
}

void
ProcessKeyPress( int ch )
{
    int Channel = 0;

    /* shifted keys act on channel 1 */
    if ( ch >= 'A' && ch <= 'Z' )
    {
        Channel = 1;
        /* change from upper to lower case */
        ch += ( 'a' - 'A' );
    }

    if ( ch == 'q' )
    {
        run = FALSE;
        return;
    }

    /* ignore if channel is not in use */
    if ( !Config.LoRaDevices[Channel].InUse )
    {
        return;
    }

    switch ( ch )
    {
        case 'f':
            Config.LoRaDevices[Channel].AFC =
                !Config.LoRaDevices[Channel].AFC;
            ChannelPrintf( Channel, 11, 24, "%s",
                           Config.LoRaDevices[Channel].AFC ? "AFC" : "   " );
            break;
        case 'a':
            ReTune( Channel, 0.1 );
            break;
        case 'z':
            ReTune( Channel, -0.1 );
            break;
        case 's':
            ReTune( Channel, 0.01 );
            break;
        case 'x':
            ReTune( Channel, -0.01 );
            break;
        case 'd':
            ReTune( Channel, 0.001 );
            break;
        case 'c':
            ReTune( Channel, -0.001 );
            break;
        default:
            //LogMessage("KeyPress %d\n", ch);
            return;
    }
}

int
prog_count( char *name )
{
    DIR *dir;
    struct dirent *ent;
    char buf[512];
    long pid;
    char pname[100] = { 0, };
    char state;
    FILE *fp = NULL;
    int Count = 0;

    if ( !( dir = opendir( "/proc" ) ) )
    {
        perror( "can't open /proc" );
        return 0;
    }

    while ( ( ent = readdir( dir ) ) != NULL )
    {
        long lpid = atol( ent->d_name );
        if ( lpid < 0 )
            continue;
        snprintf( buf, sizeof( buf ), "/proc/%ld/stat", lpid );
        fp = fopen( buf, "r" );

        if ( fp )
        {
            if ( ( fscanf( fp, "%ld (%[^)]) %c", &pid, pname, &state ) ) !=
                 3 )
            {
                printf( "fscanf failed \n" );
                fclose( fp );
                closedir( dir );
                return 0;
            }

            if ( !strcmp( pname, name ) )
            {
                Count++;
            }
            fclose( fp );
        }
    }

    closedir( dir );

    return Count;
}

int
GetTextMessageToUpload( int Channel, char *Message )
{
    DIR *dp;
    struct dirent *ep;
    int Result;

    Result = 0;

    LogMessage("Checking for SMS file ...\n");

    if ( Config.SMSFolder[0] )
    {
        dp = opendir( Config.SMSFolder );
        if ( dp != NULL )
        {
            while ( ( ep = readdir( dp ) ) && !Result )
            {
                if ( strstr( ep->d_name, ".sms" ) )
                {
                    FILE *fp;
                    char Line[256], FileName[256];
                    int FileNumber;

                    sprintf( FileName, "%s%s", Config.SMSFolder, ep->d_name );
                    sscanf( ep->d_name, "%d", &FileNumber );

                    if ( ( fp = fopen( FileName, "rt" ) ) != NULL )
                    {
                        if ( fscanf( fp, "%[^\r]", Line ) )
                        {
                            // #001,@daveake: Good Luck Tim !!\n
                            // @jonathenharty: RT @ProjectHeT: #astroPiTest The Essex Space Agency is looking forward to tweeting the real @esa! @GallarottiA: RT @ProjectHeT: #astroPiTest The Essex Space Agency is looking forward to tweeting the real @esa!
                            sprintf( Message, "#%d,%s\n", FileNumber, Line );

                            LogMessage( "UPLINK: %s", Message );
                            Result = 1;
                        }
                        else
                        {
                            LogMessage( "FAIL\n" );
                        }
                        fclose( fp );
                    }
                }
            }
            closedir( dp );
        }
    }

    return Result;
}

int
GetExternalListOfMissingSSDVPackets( int Channel, char *Message )
{
    // First, create request file
    FILE *fp;

    // LogMessage("GetExternalListOfMissingSSDVPackets()\n");

    // if ((fp = fopen("get_list.txt", "wt")) != NULL)
    {
        int i;

        // fprintf(fp, "No Message\n");
        // fclose(fp);

        // LogMessage("File created\n");

        // Now wait for uplink.txt file to appear.
        // Timeout before the end of our Tx slot if no file appears

        for ( i = 0; i < 20; i++ )
        {
            if ( ( fp = fopen( "uplink.txt", "r" ) ) )
            {
                Message[0] = '\0';
                fgets( Message, 256, fp );

                fclose( fp );

                LogMessage( "Got uplink.txt %d bytes\n", strlen( Message ) );

                // remove("get_list.txt");
                remove( "uplink.txt" );

                return strlen( Message );
            }

            usleep( 100000 );
        }

        // LogMessage("Timed out waiting for file\n");
        // remove("get_list.txt");
    }

    return 0;
}


void SendUplinkMessage( int Channel )
{
    char Message[512];

    // Decide what type of message we need to send
    if ( GetTextMessageToUpload( Channel, Message ) )
    {
        SendLoRaData( Channel, Message, 255 );
    }
    else if ( GetExternalListOfMissingSSDVPackets( Channel, Message ) )
    {
        SendLoRaData( Channel, Message, 255 );
    }
}

void
rjh_post_message( int Channel, char *buffer )
{
    if ( Config.LoRaDevices[Channel].Sending )
    {
        Config.LoRaDevices[Channel].Sending = 0;
        // LogMessage("Ch%d: End of Tx\n", Channel);

        setLoRaMode( Channel );
        SetDefaultLoRaParameters( Channel );
        startReceiving( Channel );
    }
    else
    {
        int Bytes;
        char Message[257];

        memcpy( Message + 1, buffer, 256 );

        // hexdump_buffer ("RJH Raw Data", Message, 257);

        Bytes = strlen( buffer );

        if ( Bytes > 0 )
        {
            if ( Config.LoRaDevices[Channel].ActivityLED >= 0 )
            {
                digitalWrite( Config.LoRaDevices[Channel].ActivityLED, 1 );
                LEDCounts[Channel] = 5;
            }

            if ( Message[1] == '!' )
            {
                ProcessUploadMessage( Channel, Message + 1 );
            }
            else if ( Message[1] == '^' )
            {
                ProcessCallingMessage( Channel, Message + 1 );
            }
            else if ( Message[1] == '$' )
            {
                //LogMessage("Ch %d: Uploaded message %s\n", Channel, Message+1);
                ProcessTelemetryMessage( Channel, Message + 1 );
            }
            else if ( Message[1] == '>' )
            {
                LogMessage( "Flight Controller message %d bytes = %s", Bytes,
                            Message + 1 );
            }
            else if ( Message[1] == '*' )
            {
                LogMessage( "Uplink Command message %d bytes = %s", Bytes,
                            Message + 1 );
            }
            else if ( Message[1] == 0x66 || Message[1] == 0x68 )
            {
                ProcessSSDVMessage( Channel, Message );
            }
            else
            {
                LogMessage( "Unknown packet type is %02Xh, RSSI %d\n",
                            Message[1], readRegister( Channel,
                                                      REG_PACKET_RSSI ) -
                            157 );
                ChannelPrintf( Channel, 3, 1, "Unknown Packet %d, %d bytes",
                               Message[0], Bytes );
                Config.LoRaDevices[Channel].UnknownCount++;
            }

            Config.LoRaDevices[Channel].LastPacketAt = time( NULL );

            if ( Config.LoRaDevices[Channel].InCallingMode
                 && ( Config.CallingTimeout > 0 ) )
            {
                Config.LoRaDevices[Channel].ReturnToCallingModeAt =
                    time( NULL ) + Config.CallingTimeout;
            }

            ShowPacketCounts( Channel );
        }
    }
}

int
main( int argc, char **argv )
{
    int ch;
    int LoopPeriod;
	int Channel;
    pthread_t SSDVThread, FTPThread, NetworkThread, HabitatThread,
        ServerThread;
    WINDOW *mainwin;

    if ( prog_count( "gateway" ) > 1 )
    {
        printf( "\nThe gateway program is already running!\n\n" );
        exit( 1 );
    }
	
    curl_global_init( CURL_GLOBAL_ALL );    // RJH thread safe

    mainwin = InitDisplay(  );

    // Settings for character input
    noecho(  );
    cbreak(  );
    nodelay( stdscr, TRUE );
    keypad( stdscr, TRUE );

    LEDCounts[0] = 0;
    LEDCounts[1] = 0;

    // Remove any old SSDV files
    // system("rm -f /tmp/*.bin");  

    LoadConfigFile();
    LoadPayloadFiles(  );

    int result;

    result = pipe( telem_pipe_fd );
    if ( result < 0 )
    {
        fprintf( stderr, "Error creating telemetry pipe\n" );
        return 1;
    }

    result = pipe( ssdv_pipe_fd );
    if ( result < 0 )
    {
        fprintf( stderr, "Error creating ssdv pipe\n" );
        return 1;
    }

    if ( wiringPiSetup(  ) < 0 )
    {
        fprintf( stderr, "Failed to open wiringPi\n" );
        exit( 1 );
    }

    if ( Config.LoRaDevices[0].ActivityLED >= 0 )
        pinMode( Config.LoRaDevices[0].ActivityLED, OUTPUT );
    if ( Config.LoRaDevices[1].ActivityLED >= 0 )
        pinMode( Config.LoRaDevices[1].ActivityLED, OUTPUT );
    if ( Config.InternetLED >= 0 )
        pinMode( Config.InternetLED, OUTPUT );
    if ( Config.NetworkLED >= 0 )
        pinMode( Config.NetworkLED, OUTPUT );

    setupRFM98( 0 );
    setupRFM98( 1 );

    ShowPacketCounts( 0 );
    ShowPacketCounts( 1 );


    LoopPeriod = 0;

    // Initialise the vars
    stsv.parent_status = RUNNING;
    stsv.packet_count = 0;

    if ( pthread_create( &SSDVThread, NULL, SSDVLoop, ( void * ) &stsv ) )
    {
        fprintf( stderr, "Error creating SSDV thread\n" );
        return 1;
    }

    if ( pthread_create( &FTPThread, NULL, FTPLoop, NULL ) )
    {
        fprintf( stderr, "Error creating FTP thread\n" );
        return 1;
    }


    // Initialise the vars
    htsv.parent_status = RUNNING;
    htsv.packet_count = 0;


    if ( pthread_create
         ( &HabitatThread, NULL, HabitatLoop, ( void * ) &htsv ) )
    {
        fprintf( stderr, "Error creating Habitat thread\n" );
        return 1;
    }

    // RJH close (telem_pipe_fd[0]); // Close the read side of the pipe as we are writing here

    if ( Config.ServerPort > 0 )
    {
        if ( pthread_create( &ServerThread, NULL, ServerLoop, NULL ) )
        {
            fprintf( stderr, "Error creating server thread\n" );
            return 1;
        }
    }

    if ( ( Config.NetworkLED >= 0 ) && ( Config.InternetLED >= 0 ) )
    {
        if ( pthread_create( &NetworkThread, NULL, NetworkLoop, NULL ) )
        {
            fprintf( stderr, "Error creating Network thread\n" );
            return 1;
        }
    }

    if ( ( Config.latitude > -90 ) && ( Config.longitude > -90 ) )
    {
        UploadListenerTelemetry( Config.Tracker, Config.latitude,
                                 Config.longitude, Config.antenna );
    }

    char buffer[300];
    char ssdv_buff[257];
    int message_count = 0;

    char fileName[20] = "telem.txt";
    FILE *file_telem = fopen( fileName, "r" );

    char fileName_ssdv[20] = "ssdv.bin";
    FILE *file_ssdv = fopen( fileName_ssdv, "rb" );

    LogMessage( "Starting now ...\n" );

    while ( run )               //  && message_count< 9) // RJH Used for debug
    {
        if ( ( ch = getch(  ) ) != ERR )
        {
            ProcessKeyPress( ch );
        }

        /* RJH TEST */
        if ( message_count % 10 == 9 )
        {
            if ( file_telem )
            {
                if ( fgets( buffer, sizeof( buffer ), file_telem ) )
                {
                    rjh_post_message( 1, buffer );
                }
            }
            message_count++;    // We need to increment this here or we will lock
        }
        else
        {
            if ( file_ssdv )
            {
                if ( fread( ssdv_buff, 256, 1, file_ssdv ) )
                {
                    ssdv_buff[256] = '\0';
                    rjh_post_message( 1, &ssdv_buff[1] );
                }
            }
            message_count++;    // We need to increment this here or we will lock
        }


        /* RJH TEST */
        if ( LoopPeriod > 1000 )
        {
            // Every 1 second
            time_t now;
            struct tm *tm;

            now = time( 0 );
            tm = localtime( &now );

            LoopPeriod = 0;


            for ( Channel = 0; Channel <= 1; Channel++ )
            {
                if ( Config.LoRaDevices[Channel].InUse )
                {

                    ShowPacketCounts( Channel );

                    ChannelPrintf( Channel, 12, 1, "Current RSSI = %4d   ",
                                   readRegister( Channel,
                                                 REG_CURRENT_RSSI ) - 157 );

                    // if (Config.LoRaDevices[Channel].LastPacketAt > 0)
                    // {
                    // ChannelPrintf(Channel, 6, 1, "%us since last packet   ", (unsigned int)(time(NULL) - Config.LoRaDevices[Channel].LastPacketAt));
                    // }

                    if ( Config.LoRaDevices[Channel].InCallingMode
                         && ( Config.CallingTimeout > 0 )
                         && ( Config.
                              LoRaDevices[Channel].ReturnToCallingModeAt > 0 )
                         && ( time( NULL ) >
                              Config.
                              LoRaDevices[Channel].ReturnToCallingModeAt ) )
                    {
                        Config.LoRaDevices[Channel].InCallingMode = 0;
                        Config.LoRaDevices[Channel].ReturnToCallingModeAt = 0;

                        LogMessage( "Return to calling mode\n" );

                        setLoRaMode( Channel );

                        SetDefaultLoRaParameters( Channel );

                        setMode( Channel, RF98_MODE_RX_CONTINUOUS );

                        ChannelPrintf( Channel, 1, 1,
                                       "Channel %d %sMHz  %s mode", Channel,
                                       Config.LoRaDevices[Channel].Frequency,
                                       Modes[Config.LoRaDevices
                                             [Channel].SpeedMode] );
                    }

                    if ( ( Config.LoRaDevices[Channel].UplinkTime > 0 )
                         && ( Config.LoRaDevices[Channel].UplinkCycle > 0 ) )
                    {
                        long CycleSeconds;


                        CycleSeconds =
                            ( tm->tm_hour * 3600 + tm->tm_min * 60 +
                              tm->tm_sec ) %
                            Config.LoRaDevices[Channel].UplinkCycle;

                        if ( CycleSeconds ==
                             Config.LoRaDevices[Channel].UplinkTime )
                        {
                            // LogMessage("%02d:%02d:%02d - Time to send uplink message\n", tm->tm_hour, tm->tm_min, tm->tm_sec);

                            SendUplinkMessage( Channel );
                        }
                    }

                    if ( LEDCounts[Channel]
                         && ( Config.LoRaDevices[Channel].ActivityLED >= 0 ) )
                    {
                        if ( --LEDCounts[Channel] == 0 )
                        {
                            digitalWrite( Config.LoRaDevices[Channel].
                                          ActivityLED, 0 );
                        }
                    }
                }
            }
        }

        delay( 10 );
        LoopPeriod += 10;
    }
	
	LogMessage("Disabling DIO0 ISRs\n");
	for (Channel=0; Channel<2; Channel++)
	{
		if (Config.LoRaDevices[Channel].InUse)
		{
			wiringPiISR(Config.LoRaDevices[Channel].DIO0, INT_EDGE_RISING, &DIO_Ignore_Interrupt_0);
		}
	}

    LogMessage( "Closing SSDV pipe\n" );
    close( ssdv_pipe_fd[1] );

    LogMessage( "Closing Habitat pipe\n" );
    close( telem_pipe_fd[1] );

    LogMessage( "Stopping SSDV thread\n" );
    stsv.parent_status = STOPPED;

    LogMessage( "Stopping Habitat thread\n" );
    htsv.parent_status = STOPPED;

    LogMessage( "Waiting for SSDV thread to close ...\n" );
    pthread_join( SSDVThread, NULL );
    LogMessage( "SSDV thread closed\n" );

    LogMessage( "Waiting for Habitat thread to close ...\n" );
    pthread_join( HabitatThread, NULL );
    LogMessage( "Habitat thread closed\n" );

    pthread_mutex_destroy( &var );

    // sleep (3);

    CloseDisplay( mainwin );
    curl_global_cleanup(  );    // RJH thread safe

    if ( Config.NetworkLED >= 0 )
        digitalWrite( Config.NetworkLED, 0 );
    if ( Config.InternetLED >= 0 )
        digitalWrite( Config.InternetLED, 0 );
    if ( Config.LoRaDevices[0].ActivityLED >= 0 )
        digitalWrite( Config.LoRaDevices[0].ActivityLED, 0 );
    if ( Config.LoRaDevices[1].ActivityLED >= 0 )
        digitalWrite( Config.LoRaDevices[1].ActivityLED, 0 );

    return 0;

}