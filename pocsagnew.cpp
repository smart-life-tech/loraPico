/*MIT License

Copyright (c) 2016 Galen Alderson

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

Fork and modification for rpitx (c)(F5OEO 2018)

** 11.09.2019 : Added Numeric Pager support by cuddlycheetah (github.com/cuddlycheetah)
** 14.10.2019 : Added Repeating Transmission + Single Preamble Mode

*/
#include <SPI.h>
#include <RadioLib.h>

// SX1262 pin definitions - adjust these to match your setup
#define SX1262_CS 13
#define SX1262_SCK 14
#define SX1262_MOSI 15
#define SX1262_MISO 24
#define SX1262_RST 23
#define SX1262_BUSY 18
#define SX1262_ANT 17 // Antenna switch
#define SX1262_DIO1 16 // DIO1

// Radio instance
SX1262 radio = new Module(SX1262_CS, SX1262_BUSY, SX1262_RST, -1, SPI1);

// POCSAG Constants
#define SYNC 0x7CD215D8
#define IDLE 0x7A89C197
#define FRAME_SIZE 2
#define BATCH_SIZE 16
#define PREAMBLE_LENGTH 576
#define FLAG_ADDRESS 0x000000
#define FLAG_MESSAGE 0x100000
#define FLAG_TEXT_DATA 0x3
#define FLAG_NUMERIC_DATA 0x0
#define TEXT_BITS_PER_WORD 20
#define TEXT_BITS_PER_CHAR 7
#define NUMERIC_BITS_PER_WORD 20
#define NUMERIC_BITS_PER_DIGIT 4
#define CRC_BITS 10
#define CRC_GENERATOR 0b11101101001

// Global variables - adjust as needed
uint64_t SetFrequency = 466230000L; // Frequency in Hz
int SetRate = 1200;                // Baud rate
int SetFunctionBits = 3;           // Function bits
int REPEAT_COUNT = 4;              // Repeat count
bool SetInverted = false;          // Invert modulation
bool debug = false;                // Debug flag
bool numeric = false;              // Numeric messages flag

// Char Translationtable
char *mirrorTab = new char[10]{0x00, 0x08, 0x04, 0x0c, 0x02, 0x0a, 0x06, 0x0e, 0x01, 0x09};

// Function declarations
uint32_t crc(uint32_t inputMsg);
uint32_t parity(uint32_t x);
uint32_t encodeCodeword(uint32_t msg);
uint32_t encodeASCII(uint32_t initial_offset, char *str, uint32_t *out);
char encodeDigit(char ch);
uint32_t encodeNumeric(uint32_t initial_offset, char *str, uint32_t *out);
int addressOffset(int address);
void encodeTransmission(int repeatIndex, int address, int fb, char *message, uint32_t *out);
size_t textMessageLength(int repeatIndex, int address, int numChars);
size_t numericMessageLength(int repeatIndex, int address, int numChars);
void SendFsk(uint64_t Freq, bool Inverted, int SR, bool debug, uint32_t *Message, int Size);

// CRC calculation
uint32_t crc(uint32_t inputMsg) {
    uint32_t denominator = CRC_GENERATOR << 20;
    uint32_t msg = inputMsg << CRC_BITS;

    for (int column = 0; column <= 20; column++) {
        int msgBit = (msg >> (30 - column)) & 1;
        if (msgBit != 0) {
            msg ^= denominator;
        }
        denominator >>= 1;
    }
    return msg & 0x3FF;
}

// Parity calculation
uint32_t parity(uint32_t x) {
    uint32_t p = 0;
    for (int i = 0; i < 32; i++) {
        p ^= (x & 1);
        x >>= 1;
    }
    return p;
}

// Encode codeword
uint32_t encodeCodeword(uint32_t msg) {
    uint32_t fullCRC = (msg << CRC_BITS) | crc(msg);
    uint32_t p = parity(fullCRC);
    return (fullCRC << 1) | p;
}

// Encode ASCII message
uint32_t encodeASCII(uint32_t initial_offset, char *str, uint32_t *out) {
    uint32_t numWordsWritten = 0;
    uint32_t currentWord = 0;
    uint32_t currentNumBits = 0;
    uint32_t wordPosition = initial_offset;

    while (*str != 0) {
        unsigned char c = *str;
        str++;
        for (int i = 0; i < TEXT_BITS_PER_CHAR; i++) {
            currentWord <<= 1;
            currentWord |= (c >> i) & 1;
            currentNumBits++;
            if (currentNumBits == TEXT_BITS_PER_WORD) {
                *out = encodeCodeword(currentWord | FLAG_MESSAGE);
                out++;
                currentWord = 0;
                currentNumBits = 0;
                numWordsWritten++;

                wordPosition++;
                if (wordPosition == BATCH_SIZE) {
                    *out = SYNC;
                    out++;
                    numWordsWritten++;
                    wordPosition = 0;
                }
            }
        }
    }

    if (currentNumBits > 0) {
        currentWord <<= 20 - currentNumBits;
        *out = encodeCodeword(currentWord | FLAG_MESSAGE);
        out++;
        numWordsWritten++;

        wordPosition++;
        if (wordPosition == BATCH_SIZE) {
            *out = SYNC;
            out++;
            numWordsWritten++;
            wordPosition = 0;
        }
    }

    return numWordsWritten;
}

// Encode Digit
char encodeDigit(char ch) {
    if (ch >= '0' && ch <= '9')
        return mirrorTab[ch - '0'];

    switch (ch) {
        case ' ':
            return 0x03;
        case 'u':
        case 'U':
            return 0x0d;
        case '-':
        case '_':
            return 0x0b;
        case '(':
        case '[':
            return 0x0f;
        case ')':
        case ']':
            return 0x07;
    }

    return 0x05;
}

// Encode Numeric message
uint32_t encodeNumeric(uint32_t initial_offset, char *str, uint32_t *out) {
    uint32_t numWordsWritten = 0;
    uint32_t currentWord = 0;
    uint32_t currentNumBits = 0;
    uint32_t wordPosition = initial_offset;

    while (*str != 0) {
        unsigned char c = *str;
        str++;
        for (int i = 0; i < NUMERIC_BITS_PER_DIGIT; i++) {
            currentWord <<= 1;
            char digit = encodeDigit(c);
            digit = ((digit & 1) << 3) |
                    ((digit & 2) << 1) |
                    ((digit & 4) >> 1) |
                    ((digit & 8) >> 3);

            currentWord |= (digit >> i) & 1;
            currentNumBits++;
            if (currentNumBits == NUMERIC_BITS_PER_WORD) {
                *out = encodeCodeword(currentWord | FLAG_MESSAGE);
                out++;
                currentWord = 0;
                currentNumBits = 0;
                numWordsWritten++;

                wordPosition++;
                if (wordPosition == BATCH_SIZE) {
                    *out = SYNC;
                    out++;
                    numWordsWritten++;
                    wordPosition = 0;
                }
            }
        }
    }

    if (currentNumBits > 0) {
        currentWord <<= 20 - currentNumBits;
        *out = encodeCodeword(currentWord | FLAG_MESSAGE);
        out++;
        numWordsWritten++;

        wordPosition++;
        if (wordPosition == BATCH_SIZE) {
            *out = SYNC;
            out++;
            numWordsWritten++;
            wordPosition = 0;
        }
    }

    return numWordsWritten;
}

// Address offset calculation
int addressOffset(int address) {
    return (address & 0x7) * FRAME_SIZE;
}

// Encode transmission
void encodeTransmission(int repeatIndex, int address, int fb, char *message, uint32_t *out) {
    if (repeatIndex == 0) {
        for (int i = 0; i < PREAMBLE_LENGTH / 32; i++) {
            *out = 0xAAAAAAAA;
            out++;
        }
    }

    uint32_t *start = out;

    *out = SYNC;
    out++;

    int prefixLength = addressOffset(address);
    for (int i = 0; i < prefixLength; i++) {
        *out = IDLE;
        out++;
    }

    *out = encodeCodeword(((address >> 3) << 2) | fb);
    out++;

    if (numeric) {
        out += encodeNumeric(addressOffset(address) + 1, message, out);
    } else {
        out += encodeASCII(addressOffset(address) + 1, message, out);
    }

    *out = IDLE;
    out++;

    size_t written = out - start;
    size_t padding = (BATCH_SIZE + 1) - written % (BATCH_SIZE + 1);
    for (size_t i = 0; i < padding; i++) {
        *out = IDLE;
        out++;
    }
}

// Text message length calculation
size_t textMessageLength(int repeatIndex, int address, int numChars) {
    size_t numWords = 0;
    numWords += addressOffset(address);
    numWords++;
    numWords += (numChars * TEXT_BITS_PER_CHAR + (TEXT_BITS_PER_WORD - 1)) / TEXT_BITS_PER_WORD;
    numWords++;
    numWords += BATCH_SIZE - (numWords % BATCH_SIZE);
    numWords += numWords / BATCH_SIZE;
    if (repeatIndex == 0)
        numWords += PREAMBLE_LENGTH / 32;
    return numWords;
}

// Numeric message length calculation
size_t numericMessageLength(int repeatIndex, int address, int numChars) {
    size_t numWords = 0;
    numWords += addressOffset(address);
    numWords++;
    numWords += (numChars * NUMERIC_BITS_PER_DIGIT + (NUMERIC_BITS_PER_WORD - 1)) / NUMERIC_BITS_PER_WORD;
    numWords++;
    numWords += BATCH_SIZE - (numWords % BATCH_SIZE);
    numWords += numWords / BATCH_SIZE;
    if (repeatIndex == 0)
        numWords += PREAMBLE_LENGTH / 32;
    return numWords;
}

// FSK Modulation and Transmission
void SendFsk(uint64_t Freq, bool Inverted, int SR, bool debug, uint32_t *Message, int Size) {
    float Deviation = 4500;
    int FiFoSize = 12000;
    if (debug)
        Serial.printf("Fifo Size = %d, Size = %d, Baud rate = %d\n", FiFoSize, Size, SR);

    // Calculate bit duration in microseconds
    unsigned long bitDuration = 1000000 / SR;

    // Configure radio for direct FSK mode
    int state = radio.setFrequency(Freq / 1000000.0); // Frequency in MHz
    state |= radio.setBitRate(SR);
    state |= radio.setFrequencyDeviation(Deviation / 1000.0); // Deviation in kHz
    state |= radio.setOutputPower(10);

    if (state != RADIOLIB_ERR_NONE) {
        Serial.print("Radio config failed: ");
        Serial.println(state);
        return;
    }

    // Enable TX
    digitalWrite(SX1262_ANT, HIGH);
    delay(100);

    // Transmit each bit with proper timing
    Serial.println("Starting bit-by-bit transmission...");
    for (int i = 0; i < Size; i++) {
        uint32_t word = Message[i];
        for (int j = 31; j >= 0; j--) {
            // Set frequency deviation based on bit value
            if (((word >> j) & 1) ^ Inverted) {
                radio.setFrequencyRaw(Freq + Deviation); // Mark
            } else {
                radio.setFrequencyRaw(Freq - Deviation); // Space
            }

            // Wait for bit duration
            delayMicroseconds(bitDuration);
        }
    }

    // Standby mode
    radio.standby();
    digitalWrite(SX1262_ANT, LOW);

    Serial.println("Transmission complete!");
}

void setup() {
    Serial.begin(115200);
    while (!Serial);

    // Initialize SPI
    SPI1.setRX(SX1262_MISO);
    SPI1.setCS(SX1262_CS);
    SPI1.setSCK(SX1262_SCK);
    SPI1.setTX(SX1262_MOSI);
    SPI1.begin();
    Serial.println("SPI1 initialized");
    delay(1000);

    // For XTAL (standard crystal)
    radio.setTCXO(0);

    // Initial radio setup
    int state = radio.beginFSK(
        SetFrequency / 1000000.0, // Frequency in MHz
        SetRate,                   // Bit rate
        4.5,                       // Frequency deviation
        156.2,                     // Receiver bandwidth
        10,                        // Output power
        16,                        // Preamble length
        0                          // TCXO voltage
    );

    if (state != RADIOLIB_ERR_NONE) {
        Serial.print(F("SX1262 init failed, code "));
        Serial.println(state);
        while (true);
    }
    Serial.println("SX1262 initialized");

    // Set antenna switch pin
    pinMode(SX1262_ANT, OUTPUT);
    digitalWrite(SX1262_ANT, HIGH);
    delay(1000);
    Serial.println("SX1262 ready");
}

void loop() {
    //Read in lines from STDIN.
    //Lines are in the format of address:message
    //The program will encode transmissions for each message, writing them
    //to STDOUT. It will also encode a rand amount of silence between them,
    //from 1-10 seconds in length, to act as a simulated "delay".
    char line[65536];
    char *endptr;
    //srand(time(NULL)); // time() is not available on Arduino

    int msgIndex = 0;
    size_t completeLength = 0;
    uint32_t *completeTransmission =
        (uint32_t *)malloc(sizeof(uint32_t) * 0);
    if (Serial.available() > 0){
        String input = Serial.readStringUntil('\n');
        input.toCharArray(line, sizeof(line));
        size_t colonIndex = 0;
        for (size_t i = 0; i < sizeof(line); i++) {
            if (line[i] == 0) {
                Serial.println("Malformed Line!");
                return;
            }
            if (line[i] == ':') {
                colonIndex = i;
                break;
            }
        }

        int address = (int)strtol(line, &endptr, 10);
        char *message = line + colonIndex + 1;

        // If address is followed by a letter, this set the function bits
        /*switch (*endptr) {
            case 'a':
            case 'A':
                SetFunctionBits = 0;
                break;
            case 'b':
            case 'B':
                SetFunctionBits = 1;
                break;
            case 'c':
            case 'C':
                SetFunctionBits = 2;
                break;
            case 'd':
            case 'D':
                SetFunctionBits = 3;
                break;
        }*/

        for (int x = 0; x < REPEAT_COUNT; x++) {
            size_t messageLength = numeric
                                       ? numericMessageLength(msgIndex, address, strlen(message))
                                       : textMessageLength(msgIndex, address, strlen(message));

            uint32_t *transmission =
                (uint32_t *)malloc(sizeof(uint32_t) * messageLength);

            encodeTransmission(msgIndex, address, SetFunctionBits, message, transmission);
            size_t beforeLength = completeLength + 0;
            Serial.printf("DEBUG DATA = I=%d   P=%p T=%d L=%d\n", msgIndex, completeTransmission, completeLength, messageLength);
            completeLength += messageLength;
            completeTransmission = (uint32_t *)realloc(completeTransmission, sizeof(uint32_t) * completeLength);
            for (size_t byteI = 0; byteI < messageLength; byteI++) {
                completeTransmission[beforeLength + byteI] = transmission[byteI];
            }
            free(transmission);
            msgIndex++;
        }
        SendFsk(SetFrequency, SetInverted, SetRate, debug, completeTransmission, completeLength);
        free(completeTransmission);
        completeLength = 0;
    }
}