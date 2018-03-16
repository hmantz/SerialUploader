// Serial uploader sketch for Arduino
//
// Edit constants in SerialUploader.h
//
// Credits:
//      https://github.com/robokoding/STK500
//      https://github.com/Optiboot/optiboot

#include <avr/wdt.h>
#include "SerialUploader.h"
#include "Stk500Client.h"
#include "UploaderUI.h"
#include "SerialUI.h"
#include "SDSketchSource.h"
#include "SDUI.h"

SdFat sd;
#ifdef SERIAL_UI
SerialUI ui(SERIAL_UI);
#else
SDUI ui(sd);
#endif
Stk500Client client(ui, SERIAL_TARGET);
SDSketchSource sketch(ui, sd);

enum class UploadState
{
    Start, Syncing, Working, Success, Error, ShowError, ShowSuccess
};
UploadState uploadState;

int autoBaud;
uint32_t baudRate;

void resetTarget();

bool check(StkResponse response);

bool sync(uint32_t baudRate);

UploadState upload();

void blink(uint8_t count);

void setup()
{
#ifdef DEBUG
    SERIAL_UI.begin(115300);
#endif
    wdt_enable(WDTO_8S);

    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, HIGH);

    sd.begin(SS_SD);
    if (sd.cardErrorCode()) {
#ifdef DEBUG
        SERIAL_UI.println("Initializing SD card");
#endif
        delay(10000);
    }

    if (!ui.begin()) {
#ifdef DEBUG
        SERIAL_UI.println(F("UI failed to start!"));
#endif
        delay(10000);
    }
    ui.println(F("Serial uploader starting..."));

    pinMode(PIN_RESET, OUTPUT);
    uploadState = UploadState::Start;
}

void loop()
{
    wdt_reset();
    switch (uploadState) {

        case UploadState::Start:

            if (!sketch.begin()) {
                ui.println("Could not locate .hex file on SD card.");
                uploadState = UploadState::Error;
            }
            Serial.println("Getting baud rate");
            baudRate = ui.getBaudRate();
            autoBaud = (baudRate == AUTO_BAUD_RATE) ? 0 : -1;
            uploadState = UploadState::Syncing;
            break;

        case UploadState::Syncing:
            if (autoBaud < 0) {
                uploadState = sync(baudRate) ?
                        uploadState = UploadState::Working : UploadState::Error;
            } else if (AUTO_BAUD_RATES[autoBaud] != 0ul) {
                if (sync(AUTO_BAUD_RATES[autoBaud])) {
                    uploadState = UploadState::Working;
                } else {
                    autoBaud++;
                }
            } else {
                ui.println("Could not synchronize with target board.");
                uploadState = UploadState::Error;
            }

            break;

        case UploadState::Working:
            uploadState = upload();
            break;

        case UploadState::Error:
            ui.end();
            digitalWrite(LED_BUILTIN, LOW);
            uploadState = UploadState::ShowError;
            break;

        case UploadState::ShowError:
            blink(5);
            delay(800);
            break;

        case UploadState::Success:
            ui.end();
            digitalWrite(LED_BUILTIN, LOW);
            uploadState = UploadState::ShowSuccess;
            break;

        case UploadState::ShowSuccess:
            blink(2);
            delay(800);
            break;
    }
}

bool sync(uint32_t baudRate)
{
    wdt_reset();
    client.begin(baudRate);

    resetTarget();

    ui.print(F("Synching at "));
    ui.print(baudRate);
    ui.print(F(" bps..."));
    // Sync with target
    uint8_t syncCount = 0;
    for (int i = 0; i < 15 && syncCount < 1; i++) {
        wdt_reset();
        Stk status = client.sync();
        if (status == Stk::OK) {
            syncCount++;
        }
    }
    if (syncCount < 1) {
        ui.println(F(" failed"));
        return false;
    }
    ui.println(F(" done."));
    ui.println(F("AVR device initialized and ready to accept instructions."));
    return true;
}

UploadState upload()
{
    StkResponse response;
    Stk status;

//    response = client.getSignon();
//    if (! check(response, F("Could not get signon"))) {
//        return;
//    }
//    ui.print("Signon: "); printData(response); ui.println();

    response = client.readSignature();
    if (!check(response.status, F("Could not read device signature"))) {
        return UploadState::Error;
    }
    ui.print(F("Device signature: "));
    for (int j = 0; j < response.size; ++j) {
        printByte(ui, response.data[j]);
    }
    ui.println();

    // Get parameters, for fun
    response = client.getParameter(StkParam::SW_MAJOR);
    uint8_t major = response.data[0];
    response = client.getParameter(StkParam::STK_SW_MINOR);
    ui.print(F("Bootloader version: "));
    ui.print(major, DEC);
    ui.print('.');
    ui.println(response.data[0], DEC);
    wdt_reset();

    // Program
    status = client.enterProgramming();
    if (!check(status, F("Could not enter programming mode."))) {
        return UploadState::Error;
    }
    ui.println(F("Writing flash..."));

    uint8_t hex[128];
    // byte count
    int count;
    uint16_t byteAddress = 0;
    do {
        count = sketch.readBytes(hex, 128);

        if (count < 0) {
            ui.println("SD read failure!");
            return UploadState::Error;
        }
//        if ((count % 2) != 0) {
//            ui.println("Uneven byte count received from sketch source!");
//            done = true;
//            return;
//        }
        if (count > 0) {
            status = client.loadAddress(byteAddress / 2);
            if (!check(status, F("Could not load address."))) {
                return UploadState::Error;
            }

            status = client.writeFlash((uint8_t) count, hex);
            if (!check(status, F("Could not program page!"))) {
                return UploadState::Error;
            }
            byteAddress += count;
        }
        wdt_reset();

    } while (count > 0);

    ui.print(byteAddress);
    ui.println(F(" bytes of flash written."));

    ui.println(F("Verifying flash memory against the sketch..."));

    sketch.reset();
    byteAddress = 0;
    uint8_t targetBuf[128];
    do {
        count = sketch.readBytes(hex, 128);

        if (count < 0) {
            ui.println("SD read failure!");
            return UploadState::Error;
        }
        if (count > 0) {
            status = client.loadAddress(byteAddress / 2);
            if (!check(status, F("Could not load address."))) {
                return UploadState::Error;
            }
            byteAddress += count;

            response = client.readFlash((uint8_t) count, targetBuf);
            if (!check(status, F("Could not read page!"))) {
                return UploadState::Error;
            }
            if (response.size != count) {
                ui.println(F("Read flash error: got "));
                ui.print(response.size);
                ui.print(F(" bytes out of "));
                ui.print(count);
                ui.println(F(" requested"));
                return UploadState::Error;
            }
            for (int i = 0; i < count; ++i) {
                if (hex[i] != targetBuf[i]) {
                    ui.print(F("Read flash error: data mismatch at byte "));
                    ui.println(byteAddress + i, HEX);
                    ui.print(F("Expected: 0x"));
                    ui.print(hex[i], HEX);
                    ui.print(F(", Got: 0x"));
                    ui.println(targetBuf[i], HEX);
                    return UploadState::Error;
                }
            }
        }
        wdt_reset();

    } while (count > 0);

    ui.print(byteAddress);
    ui.println(F(" bytes of flash verified."));

    status = client.leaveProgramming();
    if (!check(status, F("Could not leave programming mode."))) {
        return UploadState::Error;
    }
    ui.println("Programming done, thank you.");
//    client.end();
    return UploadState::Success;
}

void blink(uint8_t count)
{
    for (int i = 0; i < count; ++i) {
        digitalWrite(LED_BUILTIN, HIGH);
        delay(75);
        digitalWrite(LED_BUILTIN, LOW);
        delay(75);
    }
}

bool check(Stk status, FString msg)
{
    if (status != Stk::OK) {
        ui.print(msg);
        ui.print(F(": 0x"));
        ui.println((uint8_t) status, HEX);
        return false;
    }
    return true;
}

void resetTarget()
{
    ui.print(F("Resetting target board..."));
    // Reset the target board
    digitalWrite(PIN_RESET, LOW);
    delay(DELAY_RESET);
    digitalWrite(PIN_RESET, HIGH);
    delay(DELAY_RESET);
    ui.println(F(" done."));
}
