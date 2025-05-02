#include <RadioLib.h>
#include <string.h>
#include <SPI.h>

// SX1262 pin definitions
#define SX1262_CS 13   // GP13
#define SX1262_SCK 14  // GP14
#define SX1262_MOSI 15 // GP15
#define SX1262_MISO 24 // GP24
#define SX1262_RST 23  // GP23
#define SX1262_BUSY 18 // GP18
#define SX1262_ANT 17  // GP17 (Antenna switch)
#define SX1262_DIO1 16 // GP16 (DIO1)

// SX1262 has the following connections:
// NSS pin:   10
// DIO1 pin:  2
// NRST pin:  3
// BUSY pin:  9
// SX1262 radio = new Module(13, 16, 23, 18, SPI1);
// SX1262 instance with SPI1
SX1262 radio = new Module(SX1262_CS, SX1262_BUSY, SX1262_RST, -1, SPI1);

// POCSAG constants
#define FREQ 930.0       // 930 MHz
#define BITRATE 250.0    // 250 bps
#define DEVIATION 4.5    // Â±4.5 kHz
#define PREAMBLE_LEN 576 // 576 bits
#define SYNC_WORD 0x7CD215D8
#define SYNC_WORD_LEN 32
#define MAX_MESSAGE_LEN 40
#define MAX_PACKET_SIZE 255  // Maximum packet size for SX1262
// Buffer for bitstream
char bitstream[2048]; // Large enough for preamble + message

// BCH(31,21) parity generation for POCSAG
uint32_t calculate_bch_parity(uint32_t data)
{
    uint32_t parity = 0;
    uint32_t g = 0xF4B; // BCH generator polynomial
    data <<= 10;        // Shift data to align for 10 parity bits
    for (int i = 20; i >= 0; i--)
    {
        if (data & (1 << (i + 10)))
        {
            data ^= g << i;
        }
    }
    parity = data & 0x3FF; // Extract 10 parity bits
    return parity;
}

// Encode POCSAG message
void encode_pocsag(const char *address_str, const char *message, char *bitstream)
{
    // Convert address to integer
    uint32_t address = atol(address_str);
    address &= 0x1FFFFF; // 21-bit address

    // Preamble: 576 bits of alternating 1 and 0
    for (int i = 0; i < PREAMBLE_LEN; i++)
    {
        bitstream[i] = (i % 2) ? '0' : '1';
    }
    bitstream[PREAMBLE_LEN] = '\0';

    // Sync codeword: 32 bits
    char sync[33];
    snprintf(sync, sizeof(sync), "%032b", SYNC_WORD);
    strcat(bitstream, sync);

    // Address codeword: 32 bits (2 flag bits, 18 address bits, 1 function bit, 10 parity bits, 1 even parity)
    uint32_t addr_codeword = (1 << 31) | ((address << 10) & 0x7FFFC00) | (0 << 8); // Flag bit 1, function 00
    uint32_t parity = calculate_bch_parity(addr_codeword >> 10);
    addr_codeword |= parity;
    // Add even parity bit
    int ones = 0;
    for (int i = 0; i < 31; i++)
    {
        if (addr_codeword & (1 << i))
            ones++;
    }
    addr_codeword |= (ones % 2) ? 0 : 1;
    // Append to bitstream
    char addr_bits[33];
    snprintf(addr_bits, sizeof(addr_bits), "%032b", addr_codeword);
    strcat(bitstream, addr_bits);

    // Message codeword(s)
    char msg_bits[1024] = "";
    int msg_len = strlen(message);
    for (int i = 0; i < msg_len; i += 2)
    {
        uint32_t data = 0;
        // Pack 2 ASCII characters (8 bits each) into 20-bit data field
        data |= (message[i] & 0x7F) << 13;
        if (i + 1 < msg_len)
        {
            data |= (message[i + 1] & 0x7F) << 5;
        }
        uint32_t msg_codeword = (0 << 31) | (data & 0x7FFFFE0); // Flag bit 0
        parity = calculate_bch_parity(msg_codeword >> 10);
        msg_codeword |= parity;
        ones = 0;
        for (int j = 0; j < 31; j++)
        {
            if (msg_codeword & (1 << j))
                ones++;
        }
        msg_codeword |= (ones % 2) ? 0 : 1;
        snprintf(addr_bits, sizeof(addr_bits), "%032b", msg_codeword);
        strcat(msg_bits, addr_bits);
    }
    strcat(bitstream, msg_bits);
}

void setup()
{
    Serial.begin(115200);
    while (!Serial)
        ;

    // Initialize SPI with correct pins for Raspberry Pi Pico
    // Use the standard SPI.begin() method and configure pins separately
    // SPI.begin();

    // Set up GPIO pins for SPI manually
    // pinMode(SX1262_SCK, OUTPUT);
    //  pinMode(SX1262_MOSI, OUTPUT);
    // pinMode(SX1262_MISO, INPUT);
    // pinMode(SX1262_BUSY, INPUT);
    // Initialize SX1262
    SPI1.setRX(SX1262_MISO);
    SPI1.setCS(SX1262_CS);
    SPI1.setSCK(SX1262_SCK);
    SPI1.setTX(SX1262_MOSI);
    SPI1.begin();
    Serial.println("SPI1 initialized");
    delay(1000);
    // For XTAL (standard crystal)
    radio.setTCXO(0); // Set TCXO voltage to 0V (standard crystal)
    // When initializing the radio, you might need to specify a longer timeout
    // int state = radio.beginFSK(RADIOLIB_SX126X_CHIP_TYPE_SX1262, 10000); // 10 second timeout
    int state = radio.beginFSK(
        930.0, // Carrier frequency: 930 MHz
        1.2,   // Bit rate: 1.2 kbps (1200 bps)
        4.5,   // Frequency deviation: 4.5 kHz
        156.2, // Receiver bandwidth: 156.2 kHz (default)
        10,    // Output power: 10 dBm
        16,    // Preamble length: 16 bits (default)
        0      // TCXO voltage: 0 for XTAL (adjust if using TCXO)
    );
    if (state != RADIOLIB_ERR_NONE)
    {
        Serial.print(F("SX1262 init failed, code "));
        Serial.println(state);
        while (true)
            ;
    }
    Serial.println("SX1262 initialized successfully");
    // Configure SX1262 for FSK
    state = radio.setFrequency(FREQ);
    state |= radio.setBitRate(BITRATE);
    state |= radio.setFrequencyDeviation(DEVIATION);
    state |= radio.setOutputPower(10); // Low power for short range
    state |= radio.setCurrentLimit(100.0);
    state |= radio.setDataShaping(RADIOLIB_SHAPING_0_5);
    if (state != RADIOLIB_ERR_NONE)
    {
        Serial.print(F("SX1262 config failed, code "));
        Serial.println(state);
        while (true)
            ;
    }
    Serial.println("SX1262 configured successfully");
    // Set antenna switch pin (if needed)
    // Set antenna switch pin (if needed)
    pinMode(SX1262_ANT, OUTPUT);
    digitalWrite(SX1262_ANT, HIGH); // Enable TX
    delay(1000);                    // Wait for the radio to stabilize
    Serial.println("SX1262 ready");
    Serial.println("Testing SPI write...");
    state = radio.startTransmit((uint8_t *)"TEST", 4, true);
    if (state != RADIOLIB_ERR_NONE)
    {
        Serial.print("Start transmit failed, code ");
        Serial.println(state);
        return;
    }
    else{
        Serial.println("Start transmit successful");
    }
}

void loop()
{
    if (Serial.available())
    {
        String input = Serial.readStringUntil('\n');
        char *address = strtok((char *)input.c_str(), ":");
        char *message = strtok(NULL, "\n");
        
        if (address && message && strlen(message) <= MAX_MESSAGE_LEN)
        {
            Serial.print("Transmitting to address: ");
            Serial.print(address);
            Serial.print(" with message: ");
            Serial.println(message);

            encode_pocsag(address, message, bitstream);
            
            // Get total length of bitstream
            size_t totalLength = strlen(bitstream);
            Serial.print("Total bitstream length: ");
            Serial.println(totalLength);
            
            // Enable TX
            digitalWrite(SX1262_ANT, HIGH);
            delay(100); // Give time for the switch to settle
            
            // Send bitstream in chunks
            size_t sentBytes = 0;
            bool transmissionError = false;
            
            while (sentBytes < totalLength && !transmissionError) {
                // Calculate size of next chunk
                size_t chunkSize = min(MAX_PACKET_SIZE, totalLength - sentBytes);
                
                // Create temporary buffer for this chunk
                uint8_t chunk[MAX_PACKET_SIZE];
                memcpy(chunk, bitstream + sentBytes, chunkSize);
                
                Serial.print("Sending chunk of size: ");
                Serial.println(chunkSize);
                
                // Transmit this chunk
                int state = radio.transmit(chunk, chunkSize, true);
                
                if (state == RADIOLIB_ERR_NONE) {
                    Serial.print("Chunk sent successfully: ");
                    Serial.print(sentBytes);
                    Serial.print(" to ");
                    Serial.println(sentBytes + chunkSize - 1);
                    sentBytes += chunkSize;
                } else {
                    Serial.print("Transmission error: ");
                    Serial.println(state);
                    transmissionError = true;
                }
                
                // Small delay between chunks
                delay(100);
            }
            
            digitalWrite(SX1262_ANT, LOW); // Disable TX
            
            if (!transmissionError) {
                Serial.println("Transmission complete! Message sent successfully.");
            }
        }
        else
        {
            Serial.println("Error: Invalid message format or message too long");
        }
    }
}