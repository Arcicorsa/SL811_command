#include <Arduino.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

// ==========================================
// SL811HS - Arduino Due USB Host Tester
// ==========================================

#define PIN__SPARE_CS 24
#define PIN__INT6     26
#define PIN__IORD     28
#define PIN__IOWR     30
#define PIN_A2        32
#define PIN__RESET    50

#define ENUM_MAX_RETRIES  8

const uint8_t DATA_PINS[8] = {
  48, 46, 44, 42, 40, 38, 36, 34
};

char commandBuffer[64];
uint8_t commandPos = 0;

// Set only after a successful Poseidon initialization with a connected
// Full-Speed device. ENUM8 requires this state and never reinitializes USB.
bool poseidonReady = false;

// Set only after ENUM18 has verified that the device answers at address 1.
bool enumAddress1Ready = false;
uint8_t enumStoredEp0PacketSize = 64;
uint8_t enumStoredDeviceDescriptor[18] = {0};

// SL811 registers
#define REG_CTRL       0x00
#define REG_BASE       0x01
#define REG_LENGTH     0x02
#define REG_PID        0x03
#define REG_ADDR       0x04
#define REG_CTRL1      0x05
#define REG_INTENABLE  0x06
#define REG_INTSTATUS  0x0D
#define REG_REVISION   0x0E
#define REG_CTRL2      0x0F

// ==========================================
// DATA BUS
// ==========================================

void dataBusOutput() {
  for (int i = 0; i < 8; i++)
    pinMode(DATA_PINS[i], OUTPUT);
}

void dataBusInput() {
  for (int i = 0; i < 8; i++)
    pinMode(DATA_PINS[i], INPUT);
}

void dataBusWrite(uint8_t value) {
  for (int i = 0; i < 8; i++)
    digitalWrite(DATA_PINS[i], (value >> i) & 1);
}

uint8_t dataBusRead() {
  uint8_t value = 0;

  for (int i = 0; i < 8; i++) {
    if (digitalRead(DATA_PINS[i]))
      value |= (1 << i);
  }

  return value;
}

// ==========================================
// REGISTER ACCESS
// ==========================================

void slWriteRegister(uint8_t address, uint8_t value) {

  dataBusOutput();

  digitalWrite(PIN__SPARE_CS, LOW);
  digitalWrite(PIN_A2, LOW);

  dataBusWrite(address);

  digitalWrite(PIN__IOWR, LOW);
  delayMicroseconds(1);
  digitalWrite(PIN__IOWR, HIGH);
  delayMicroseconds(1);

  digitalWrite(PIN_A2, HIGH);

  dataBusWrite(value);

  digitalWrite(PIN__IOWR, LOW);
  delayMicroseconds(1);
  digitalWrite(PIN__IOWR, HIGH);
  delayMicroseconds(1);

  digitalWrite(PIN__SPARE_CS, HIGH);
}

uint8_t slReadRegister(uint8_t address) {

  uint8_t value;

  dataBusOutput();

  digitalWrite(PIN__SPARE_CS, LOW);
  digitalWrite(PIN_A2, LOW);

  dataBusWrite(address);

  digitalWrite(PIN__IOWR, LOW);
  delayMicroseconds(1);
  digitalWrite(PIN__IOWR, HIGH);
  delayMicroseconds(1);

  dataBusInput();

  digitalWrite(PIN_A2, HIGH);
  delayMicroseconds(1);

  digitalWrite(PIN__IORD, LOW);
  delayMicroseconds(1);

  value = dataBusRead();

  digitalWrite(PIN__IORD, HIGH);
  delayMicroseconds(1);

  digitalWrite(PIN__SPARE_CS, HIGH);

  return value;
}

// ==========================================
// RAM ACCESS
// ==========================================

void slWriteRAM(uint8_t address, uint8_t value) {
  slWriteRegister(address, value);
}

uint8_t slReadRAM(uint8_t address) {
  return slReadRegister(address);
}

// ==========================================
// HARDWARE RESET
// ==========================================

void slReset() {
  enumAddress1Ready = false;
  digitalWrite(PIN__RESET, LOW);
  delay(10);
  digitalWrite(PIN__RESET, HIGH);
  delay(10);
}

// ==========================================
// USB INITIALIZATION
// ==========================================

void usbInit() {

  Serial.println();
  Serial.println("Initializing SL811 USB host...");

  slReset();

  uint8_t revision = slReadRegister(REG_REVISION);

  Serial.print("SL811 revision: 0x");
  Serial.println(revision, HEX);

  if (revision != 0x20) {
    Serial.println("WARNING: Unexpected SL811 revision!");
  }

  // USB engine reset
  slWriteRegister(REG_CTRL1, 0x08);
  delay(60);

  // Host mode, Full-Speed
  slWriteRegister(REG_CTRL2, 0xAE);
  slWriteRegister(REG_REVISION, 0xE0);

  // Release USB reset and enable SOF
  slWriteRegister(REG_CTRL1, 0x01);

  // Enable USB-A DONE and change interrupts
  slWriteRegister(REG_INTENABLE, 0x21);

  // Clear pending interrupt flags
  slWriteRegister(REG_INTSTATUS, 0xFF);

  delay(20);

  Serial.println("USB host initialized.");
}

// ==========================================
// USB RESET
// ==========================================

void usbResetDevice() {

  Serial.println("USB bus reset...");

  slWriteRegister(REG_CTRL1, 0x08);
  delay(60);

  slWriteRegister(REG_CTRL1, 0x01);
  delay(100);

  slWriteRegister(REG_INTSTATUS, 0xFF);

  Serial.println("USB reset released.");
}


// ==========================================
// POSEIDON RESET / INITIALIZATION SEQUENCE
// ==========================================
// This follows sl811hs_ResetHW(), sl811hs_ResetUSB()
// and sl811hs_PortScan() from the supplied Poseidon source.
// The low-level bus functions above are unchanged.
// Register operations are buffered and printed AFTER the sequence,
// so Serial output cannot insert long delays between controller writes.

#define POSEIDON_INT_FULLSPEED  0x80
#define POSEIDON_INT_DETECT     0x40
#define POSEIDON_INT_CHANGED    0x20
#define POSEIDON_INT_USB_B      0x02
#define POSEIDON_INT_USB_A      0x01

#define POSEIDON_CTRL1_RESET    0x08
#define POSEIDON_CTRL1_LOWSPD   0x20
#define POSEIDON_CTRL1_SOF      0x01
#define POSEIDON_CTRL2_MASTER   0x80
#define POSEIDON_CTRL2_LOWSPD   0x40
#define POSEIDON_PID_SOF        0x50
#define POSEIDON_XFER_A_BASE    16
#define POSEIDON_XFER_B_BASE    0   // Default source build: ENABLE_B not defined

#define POSEIDON_LOG_MAX 40

struct PoseidonLogEntry {
  char type;                 // 'W' write, 'R' read, 'D' delay, 'M' marker
  uint8_t reg;
  uint8_t value;
  const char *description;
};

PoseidonLogEntry poseidonLog[POSEIDON_LOG_MAX];
uint8_t poseidonLogCount = 0;
bool poseidonLogOverflow = false;

void poseidonLogAdd(char type, uint8_t reg, uint8_t value, const char *description) {
  if (poseidonLogCount < POSEIDON_LOG_MAX) {
    poseidonLog[poseidonLogCount].type = type;
    poseidonLog[poseidonLogCount].reg = reg;
    poseidonLog[poseidonLogCount].value = value;
    poseidonLog[poseidonLogCount].description = description;
    poseidonLogCount++;
  } else {
    poseidonLogOverflow = true;
  }
}

void poseidonMark(const char *description) {
  poseidonLogAdd('M', 0, 0, description);
}

void poseidonWrite(uint8_t reg, uint8_t value, const char *description) {
  slWriteRegister(reg, value);
  poseidonLogAdd('W', reg, value, description);
}

uint8_t poseidonRead(uint8_t reg, const char *description) {
  uint8_t value = slReadRegister(reg);
  poseidonLogAdd('R', reg, value, description);
  return value;
}

void poseidonDelay(uint16_t ms) {
  poseidonLogAdd('D', 0, (uint8_t)ms, "delay in milliseconds");
  delay(ms);
}

void poseidonPrintLog() {
  Serial.println();
  Serial.println("POSEIDON REGISTER TRACE");
  Serial.println("-----------------------");

  for (uint8_t i = 0; i < poseidonLogCount; i++) {
    const PoseidonLogEntry &e = poseidonLog[i];

    if (e.type == 'M') {
      Serial.println();
      Serial.println(e.description);
    } else if (e.type == 'D') {
      Serial.print("  DELAY ");
      Serial.print(e.value);
      Serial.println(" ms");
    } else {
      Serial.print("  ");
      Serial.print(e.type);
      Serial.print(" 0x");
      if (e.reg < 0x10) Serial.print("0");
      Serial.print(e.reg, HEX);
      Serial.print(e.type == 'W' ? " <- 0x" : " -> 0x");
      if (e.value < 0x10) Serial.print("0");
      Serial.print(e.value, HEX);
      Serial.print("  ");
      Serial.println(e.description);
    }
  }

  if (poseidonLogOverflow)
    Serial.println("WARNING: trace buffer overflow; some entries were omitted.");
}

void poseidonInitialization() {
  poseidonReady = false;
  enumAddress1Ready = false;
  poseidonLogCount = 0;
  poseidonLogOverflow = false;

  Serial.println();
  Serial.println("========================================");
  Serial.println(" POSEIDON SL811HS INITIALIZATION SEQUENCE");
  Serial.println("========================================");
  Serial.println("Executing sequence; trace will print after completion.");
  Serial.println("No USB enumeration or SETUP/IN transfers are issued.");

  // sl811hs_ResetHW(): read and validate silicon revision.
  poseidonMark("[1] ResetHW - read silicon revision");
  uint8_t revision = poseidonRead(REG_REVISION, "HW revision (read alias of SOFLOW)");

  if (revision != 0x20) {
    poseidonPrintLog();
    Serial.println();
    Serial.println("ERROR: Poseidon expects revision 0x20.");
    Serial.println("Sequence aborted; no initialization writes performed.");
    return;
  }

  // ResetHW(): initialize controller.
  poseidonMark("[2] ResetHW - controller initialization");
  poseidonWrite(REG_CTRL2, 0xAE, "MASTER | SOF_HIGH(0x2E)");
  poseidonWrite(REG_INTSTATUS, 0xFF, "clear all pending W1C status flags");
  poseidonWrite(REG_INTENABLE, 0x00, "disable interrupts");
  poseidonDelay(40);

  // ResetUSB(TRUE): begin USB bus reset.
  poseidonMark("[3] ResetUSB(TRUE) - assert USB reset");
  poseidonWrite(REG_INTENABLE, 0x00, "interrupts disabled (repeated by source)");
  poseidonWrite(REG_CTRL1, POSEIDON_CTRL1_RESET, "USB_RESET asserted");
  poseidonDelay(50);

  // ResetUSB(FALSE): release reset, then PortScan.
  poseidonMark("[4] ResetUSB(FALSE) - release USB reset");
  poseidonWrite(REG_CTRL1, 0x00, "USB_RESET released; SOF not yet enabled");

  // PortScan(): clear W1C flags BEFORE reading live state.
  poseidonMark("[5] PortScan - clear flags and inspect live port state");
  poseidonWrite(REG_INTSTATUS, 0xFF, "clear all W1C flags before sampling port");
  uint8_t state = poseidonRead(REG_INTSTATUS, "live port state after clear");

  bool connected = ((state & POSEIDON_INT_DETECT) == 0);
  bool fullSpeed = ((state & POSEIDON_INT_FULLSPEED) != 0);

  if (!connected) {
    // Exact disconnect branch in sl811hs_PortScan().
    poseidonMark("PortScan branch: DISCONNECTED");
    poseidonWrite(REG_INTSTATUS, POSEIDON_INT_DETECT, "clear DETECT bit");
    uint8_t verify = poseidonRead(REG_INTSTATUS, "verify DETECT cleared");

    if (verify & POSEIDON_INT_DETECT)
      poseidonWrite(REG_INTSTATUS, 0xFF, "second clear, as in Poseidon source");
  } else {
    // Exact connect branch in sl811hs_PortScan().
    poseidonMark("PortScan branch: CONNECTED");

    uint8_t ctrl1 = 0;
    uint8_t ctrl2 = 0;

    if (!fullSpeed) {
      ctrl1 |= POSEIDON_CTRL1_LOWSPD;
      ctrl2 |= POSEIDON_CTRL2_LOWSPD;
    }

    poseidonWrite(REG_CTRL2,
                  ctrl2 | POSEIDON_CTRL2_MASTER | 0x2E,
                  "MASTER | optional LOW_SPEED | SOF_HIGH(0x2E)");
    poseidonWrite(REG_REVISION, 0xE0,
                  "SOFLOW = 0xE0 (write alias of HW revision)");
    poseidonWrite(REG_CTRL1, ctrl1 | POSEIDON_CTRL1_SOF,
                  "SOF_ENABLE | optional LOW_SPEED");
  }

  // ResetUSB(FALSE): reinitialize host channels when connected.
  // In the repository's default build (ENABLE_B undefined), channel-B
  // buffer base is 0; channel-A buffer base is 16.
  if (connected) {
    poseidonMark("[6] ResetUSB(FALSE) - initialize host channels A and B");

    poseidonWrite(REG_BASE, POSEIDON_XFER_A_BASE, "Channel A buffer base = 16");
    poseidonWrite(REG_LENGTH, 0x00, "Channel A length = 0");
    poseidonWrite(REG_PID, POSEIDON_PID_SOF, "Channel A PID = SOF, endpoint 0");
    poseidonWrite(REG_ADDR, 0x00, "Channel A device address = 0");
    poseidonWrite(REG_CTRL, 0x01, "Channel A ARM (source writes ARM only)");

    poseidonWrite(REG_BASE + 8, POSEIDON_XFER_B_BASE, "Channel B buffer base = 0");
    poseidonWrite(REG_LENGTH + 8, 0x00, "Channel B length = 0");
    poseidonWrite(REG_PID + 8, 0x00, "Channel B PID/endpoint = 0");
    poseidonWrite(REG_ADDR + 8, 0x00, "Channel B device address = 0");
    poseidonWrite(REG_CTRL + 8, 0x00, "Channel B control = 0");
  } else {
    poseidonMark("[6] Channel initialization skipped (disconnected branch)");
  }

  // ResetUSB(FALSE): enable changed + USB-B + USB-A interrupts.
  poseidonMark("[7] ResetUSB(FALSE) - enable interrupts");
  poseidonWrite(REG_INTENABLE,
                POSEIDON_INT_CHANGED | POSEIDON_INT_USB_B | POSEIDON_INT_USB_A,
                "CHANGED | USB_B | USB_A = 0x23");

  // ENUM8 is deliberately limited to connected Full-Speed devices.
  poseidonReady = connected && fullSpeed;

  // All controller writes are complete before any detailed serial trace.
  poseidonPrintLog();

  Serial.println();
  Serial.println("========================================");
  Serial.println(" POSEIDON SEQUENCE FINISHED");
  Serial.println("========================================");
  Serial.print("Port result: ");
  Serial.println(connected ? (fullSpeed ? "CONNECTED / FULL-SPEED" : "CONNECTED / LOW-SPEED") : "DISCONNECTED");
  Serial.print("Raw INTSTATUS sample: 0x");
  if (state < 0x10) Serial.print("0");
  Serial.println(state, HEX);
  Serial.println("Note: this command does not enumerate the USB device.");
}

// ==========================================
// POSEIDON-STYLE CHANNEL A TRANSFERS
// ==========================================
// This path is intentionally separate from the legacy MOUSE test below.
// It does NOT call usbInit() or issue another USB bus reset.
// The transfer programming order and SOF synchronization check follow
// sl811hs_XferIssue() in the supplied Poseidon source.

#define ENUM_INT_USB_A_DONE  0x01
#define ENUM_INT_USB_A       0x01
#define ENUM_INT_DETECT      0x40
#define ENUM_HOST_STALL      0x80
#define ENUM_HOST_NAK        0x40
#define ENUM_HOST_OVERFLOW   0x20
#define ENUM_HOST_SETUP      0x10
#define ENUM_HOST_SEQ        0x08
#define ENUM_HOST_TIMEOUT    0x04
#define ENUM_HOST_ERROR      0x02
#define ENUM_HOST_ACK        0x01

#define ENUM_HOSTCTRL_PREAMBLE 0x80
#define ENUM_HOSTCTRL_DATA1    0x40
#define ENUM_HOSTCTRL_SYNCSOF  0x20
#define ENUM_HOSTCTRL_DIR_OUT  0x04
#define ENUM_HOSTCTRL_ENABLE   0x02
#define ENUM_HOSTCTRL_ARM      0x01

#define ENUM_PID_SETUP 0x0D
#define ENUM_PID_IN    0x09
#define ENUM_PID_OUT   0x01
#define ENUM_DEVICE_ADDRESS 0x00
#define ENUM_NEW_DEVICE_ADDRESS 0x01
#define ENUM_EP0       0x00
#define ENUM_BUFFER_A  0x10
#define ENUM_WAIT_MS   500

struct EnumTransferResult {
  bool started;
  bool done;
  bool ack;
  bool sequenceApplicable; // HOSTSTATUS.SEQ is checked for IN data returned by the device
  bool sequenceOK;
  uint8_t irqStatus;
  uint8_t hostStatus;
  uint8_t txLeft;
  uint8_t sofHigh;
  uint8_t hostControl;
  uint32_t attempts;      // Number of channel-A transfers actually issued
  uint32_t attemptLimit;  // 0 means time-limited (no fixed attempt cap)
  uint32_t nakCount;      // Number of NAK responses; wide enough for long BOT waits
};

void printHexByte(uint8_t value) {
  if (value < 0x10) Serial.print('0');
  Serial.print(value, HEX);
}

void printHostStatus(uint8_t status) {
  Serial.print("HOSTSTATUS=0x");
  printHexByte(status);
  Serial.print(" [");
  bool separator = false;
  if (status & ENUM_HOST_ACK)      { Serial.print("ACK"); separator = true; }
  if (status & ENUM_HOST_NAK)      { if (separator) Serial.print('|'); Serial.print("NAK"); separator = true; }
  if (status & ENUM_HOST_STALL)    { if (separator) Serial.print('|'); Serial.print("STALL"); separator = true; }
  if (status & ENUM_HOST_OVERFLOW) { if (separator) Serial.print('|'); Serial.print("OVERFLOW"); separator = true; }
  if (status & ENUM_HOST_SETUP)    { if (separator) Serial.print('|'); Serial.print("SETUP"); separator = true; }
  if (status & ENUM_HOST_SEQ)      { if (separator) Serial.print('|'); Serial.print("SEQ=1"); separator = true; }
  if (status & ENUM_HOST_TIMEOUT)  { if (separator) Serial.print('|'); Serial.print("TIMEOUT"); separator = true; }
  if (status & ENUM_HOST_ERROR)    { if (separator) Serial.print('|'); Serial.print("ERROR"); separator = true; }
  if (!separator) Serial.print("no status bits");
  Serial.println(']');
}

bool enumClearADone() {
  // USB-A DONE is W1C. Do not clear SOF_TIMER or other live port flags.
  uint8_t before = slReadRegister(REG_INTSTATUS);
  if (before & ENUM_INT_USB_A_DONE)
    slWriteRegister(REG_INTSTATUS, ENUM_INT_USB_A_DONE);

  uint8_t after = slReadRegister(REG_INTSTATUS);
  return ((after & ENUM_INT_USB_A_DONE) == 0);
}

bool enumWaitADone(uint16_t timeoutMs, uint8_t &irqStatus) {
  uint32_t start = millis();

  while ((uint32_t)(millis() - start) < timeoutMs) {
    uint8_t status = slReadRegister(REG_INTSTATUS);

    if (status & ENUM_INT_DETECT) {
      irqStatus = status;
      return false; // Detect bit set means disconnected in Poseidon's logic.
    }

    if (status & ENUM_INT_USB_A_DONE) {
      irqStatus = status;
      slWriteRegister(REG_INTSTATUS, ENUM_INT_USB_A_DONE);
      return true;
    }

    delayMicroseconds(100);
  }

  irqStatus = slReadRegister(REG_INTSTATUS);
  return false;
}

EnumTransferResult enumIssueChannelAEndpoint(uint8_t pid,
                                             uint8_t endpoint,
                                             bool directionOut,
                                             bool data1,
                                             uint8_t deviceAddress,
                                             uint8_t buffer,
                                             uint8_t length,
                                             const uint8_t *outData) {
  EnumTransferResult result;
  result.started = false;
  result.done = false;
  result.ack = false;
  result.sequenceApplicable = false;
  result.sequenceOK = false;
  result.irqStatus = 0;
  result.hostStatus = 0;
  result.txLeft = 0xFF;
  result.sofHigh = 0;
  result.hostControl = 0;
  result.attempts = 0;
  result.attemptLimit = ENUM_MAX_RETRIES;
  result.nakCount = 0;

  // Clear old USB-A DONE before programming this transfer.
  // This is done silently so there is no Serial delay near the SOF sync check.
  if (!enumClearADone()) {
    result.hostStatus = slReadRegister(REG_PID);
    result.txLeft = slReadRegister(REG_ADDR);
    result.irqStatus = slReadRegister(REG_INTSTATUS);
    return result;
  }

  // Poseidon XferIssue(): load OUT payload into the channel buffer first.
  if (directionOut && length > 0 && outData != NULL) {
    for (uint8_t i = 0; i < length; i++)
      slWriteRAM(buffer + i, outData[i]);
  }

  // Poseidon XferIssue(): channel A registers, in source order.
  slWriteRegister(REG_BASE, buffer);
  slWriteRegister(REG_LENGTH, length);
  // SL811HS Host PID/Device Endpoint register: PID in bits 7:4,
  // endpoint number in bits 3:0. Control transfers use endpoint 0;
  // the HID mouse interrupt pipe below uses endpoint 2.
  slWriteRegister(REG_PID, (uint8_t)((pid << 4) | (endpoint & 0x0F)));
  slWriteRegister(REG_ADDR, deviceAddress);

  // Poseidon Errata 1.5 workaround: synchronize near the next SOF.
  // For Full-Speed, ticks = (len >> 3) + 3.
  result.sofHigh = slReadRegister(REG_CTRL2); // read alias = SOF high counter
  uint8_t ticks = (uint8_t)((length >> 3) + 3);

  uint8_t control = 0;
  if (directionOut) control |= ENUM_HOSTCTRL_DIR_OUT;
  if (data1) control |= ENUM_HOSTCTRL_DATA1;
  control |= ENUM_HOSTCTRL_ENABLE | ENUM_HOSTCTRL_ARM;

  if (result.sofHigh <= ticks)
    control |= ENUM_HOSTCTRL_SYNCSOF;

  result.hostControl = control;

  // Last write arms the transfer, matching Poseidon's XferIssue().
  // No Serial output occurs between SOFHIGH read and this ARM write.
  slWriteRegister(REG_CTRL, control);
  result.started = true;

  result.done = enumWaitADone(ENUM_WAIT_MS, result.irqStatus);
  result.hostStatus = slReadRegister(REG_PID);  // read alias = HOSTSTATUS
  result.txLeft = slReadRegister(REG_ADDR);     // read alias = HOSTTXLEFT

  if (!result.done)
    return result;

  result.ack = ((result.hostStatus & ENUM_HOST_ACK) != 0);

  // HOSTSTATUS.SEQ reports the DATA PID observed on an IN data packet.
  // For host OUT (including SETUP and zero-length STATUS OUT), the DATA
  // toggle is selected by HOSTCTRL bit 6; the device replies with ACK, not
  // a DATA packet whose toggle could be checked in HOSTSTATUS.
  result.sequenceApplicable = !directionOut;
  if (result.sequenceApplicable) {
    bool seq = ((result.hostStatus & ENUM_HOST_SEQ) != 0);
    result.sequenceOK = (seq == data1);
  } else {
    result.sequenceOK = true; // not applicable to a host OUT transaction
  }
  return result;
}

// Preserve the existing endpoint-0 API used by ENUM18 and IDENTIFY.
EnumTransferResult enumIssueChannelA(uint8_t pid,
                                     bool directionOut,
                                     bool data1,
                                     uint8_t deviceAddress,
                                     uint8_t buffer,
                                     uint8_t length,
                                     const uint8_t *outData) {
  return enumIssueChannelAEndpoint(pid, ENUM_EP0, directionOut, data1,
                                   deviceAddress, buffer, length, outData);
}

EnumTransferResult enumIssueChannelA_with_retry(uint8_t pid,
                                               bool directionOut,
                                               bool data1,
                                               uint8_t deviceAddress,
                                               uint8_t buffer,
                                               uint8_t length,
                                               const uint8_t *outData) {
  EnumTransferResult result;
  uint8_t attempts = 0;
  uint8_t nakCount = 0;

  for (int attempt = 0; attempt < ENUM_MAX_RETRIES; attempt++) {
    // Count every actual call, including one that fails before ARM.
    attempts++;
    result = enumIssueChannelA(pid, directionOut, data1, deviceAddress,
                               buffer, length, outData);
    result.attempts = attempts;

    if (!result.started || !result.done) {
      // Timeout, disconnect, or failure to clear the previous DONE: stop.
      result.nakCount = nakCount;
      break;
    }

    if (result.ack && result.sequenceOK) {
      result.nakCount = nakCount;
      return result;
    }

    // Only NAK is treated as a transient response and retried.
    if (result.hostStatus & ENUM_HOST_NAK) {
      nakCount++;
      result.nakCount = nakCount;
      delayMicroseconds(200);
      continue;
    }

    // STALL, toggle mismatch, or another error: do not retry.
    result.nakCount = nakCount;
    break;
  }

  // Preserve the total attempt and NAK counters in the final result.
  result.attempts = attempts;
  result.nakCount = nakCount;
  return result;
}

void enumPrintTransferResult(const char *label,
                             const EnumTransferResult &result,
                             uint8_t pid,
                             bool directionOut,
                             bool data1,
                             uint8_t length) {
  Serial.println();
  Serial.print("--- ");
  Serial.print(label);
  Serial.println(" ---");

  Serial.print("PID=0x");
  printHexByte((uint8_t)(pid << 4));
  Serial.print("  DIR=");
  Serial.print(directionOut ? "OUT" : "IN");
  Serial.print("  DATA=");
  Serial.print(data1 ? 1 : 0);
  Serial.print("  LEN=");
  Serial.print(length);
  Serial.print("  SOFHIGH=");
  printHexByte(result.sofHigh);
  Serial.print("  SYNC_SOF=");
  Serial.print((result.hostControl & ENUM_HOSTCTRL_SYNCSOF) ? "yes" : "no");
  Serial.print("  HOSTCTRL=0x");
  printHexByte(result.hostControl);
  Serial.println();

  Serial.print("ARM issued: ");
  Serial.println(result.started ? "yes" : "no");
  Serial.print("USB-A DONE: ");
  Serial.println(result.done ? "yes" : "no");

  Serial.print("Attempts: ");
  Serial.print((unsigned long)result.attempts);
  if (result.attemptLimit != 0) {
    Serial.print(" / ");
    Serial.println((unsigned long)result.attemptLimit);
  } else {
    Serial.println(" (time-limited; no attempt cap)");
  }
  Serial.print("NAK responses: ");
  Serial.println((unsigned long)result.nakCount);

  Serial.print("INTSTATUS at completion/wait end: 0x");
  printHexByte(result.irqStatus);
  Serial.println();
  printHostStatus(result.hostStatus);
  Serial.print("HOSTTXLEFT=");
  Serial.println(result.txLeft);

  if (!result.started) {
    Serial.println("ERROR: stale USB-A DONE could not be cleared before ARM.");
  } else if (!result.done) {
    if (result.irqStatus & ENUM_INT_DETECT)
      Serial.println("Transfer stopped: device disconnect detected.");
    else
      Serial.println("TIMEOUT: USB-A DONE was not observed within 500 ms.");
  } else if (!result.ack) {
    Serial.println("Transfer completed, but HOSTSTATUS does not contain ACK.");
  } else if (result.sequenceApplicable && !result.sequenceOK) {
    Serial.print("IN DATA toggle mismatch: expected SEQ=");
    Serial.print(data1 ? 1 : 0);
    Serial.print(", got SEQ=");
    Serial.println((result.hostStatus & ENUM_HOST_SEQ) ? 1 : 0);
  } else if (!result.sequenceApplicable) {
    Serial.println("Transfer ACK valid; HOSTSTATUS.SEQ check is not applicable to host OUT.");
  } else {
    Serial.println("Transfer ACK and IN DATA toggle are valid.");
  }
}

void poseidonEnum8() {
  Serial.println();
  Serial.println("========================================");
  Serial.println(" FIRST CONTROL TRANSFER - GET DESCRIPTOR");
  Serial.println("========================================");
  Serial.println("Uses the existing Poseidon initialization; no USB reset is issued.");

  if (!poseidonReady) {
    Serial.println("ERROR: Run POSEIDON first and confirm a connected Full-Speed device.");
    return;
  }

  // Check that the live controller state still matches the initialization.
  uint8_t ctrl1 = slReadRegister(REG_CTRL1);
  uint8_t intEnable = slReadRegister(REG_INTENABLE);
  uint8_t port = slReadRegister(REG_INTSTATUS);

  Serial.print("CONTROL1=0x"); printHexByte(ctrl1);
  Serial.print("  INTENABLE=0x"); printHexByte(intEnable);
  Serial.print("  PORT/INTSTATUS=0x"); printHexByte(port);
  Serial.println();

  if (!(ctrl1 & 0x01) || !(intEnable & ENUM_INT_USB_A) ||
      (port & ENUM_INT_DETECT) || !(port & 0x80)) {
    Serial.println("ERROR: SOF, USB-A interrupt, or Full-Speed connection state is not valid.");
    Serial.println("Run POSEIDON again before retrying.");
    return;
  }

  // USB reset recovery time has elapsed in normal use, but enforce at least
  // 10 ms from the start of this command before the first SETUP packet.
  Serial.println("Waiting 50 ms for USB reset recovery...");
  delay(50);

  // Standard GET_DESCRIPTOR(Device), address 0, endpoint 0, wLength = 8.
  const uint8_t setupPacket[8] = {
    0x80, 0x06, 0x00, 0x01,
    0x00, 0x00, 0x08, 0x00
  };

  EnumTransferResult setupResult = enumIssueChannelA_with_retry(
    ENUM_PID_SETUP, true, false, ENUM_DEVICE_ADDRESS,
    ENUM_BUFFER_A, 8, setupPacket);

  if (!setupResult.done || !setupResult.ack || !setupResult.sequenceOK) {
    enumPrintTransferResult("SETUP: GET_DESCRIPTOR(Device), wLength=8",
                            setupResult, ENUM_PID_SETUP, true, false, 8);
    Serial.println("STOP: SETUP stage failed; IN stage was not issued.");
    return;
  }

  // For a control read, the first data packet is DATA1 after SETUP DATA0.
  // There is deliberately no Serial printing between SETUP ACK and this IN.
  EnumTransferResult inResult = enumIssueChannelA_with_retry(
    ENUM_PID_IN, false, true, ENUM_DEVICE_ADDRESS,
    ENUM_BUFFER_A, 8, NULL);

  enumPrintTransferResult("SETUP: GET_DESCRIPTOR(Device), wLength=8",
                          setupResult, ENUM_PID_SETUP, true, false, 8);
  enumPrintTransferResult("IN: device descriptor, first packet, DATA1",
                          inResult, ENUM_PID_IN, false, true, 8);

  if (!inResult.done || !inResult.ack || !inResult.sequenceOK) {
    Serial.println("STOP: IN stage failed.");
    Serial.println("HOSTSTATUS and HOSTTXLEFT above are the key diagnostics.");
    return;
  }

  if (inResult.txLeft > 8) {
    Serial.println("ERROR: HOSTTXLEFT is greater than the requested length.");
    return;
  }

  uint8_t received = 8 - inResult.txLeft;
  Serial.print("Received bytes (8 - HOSTTXLEFT): ");
  Serial.println(received);

  if (received == 0) {
    Serial.println("No descriptor payload was received despite ACK.");
    return;
  }

  uint8_t descriptor[8];
  Serial.println("Descriptor bytes:");
  for (uint8_t i = 0; i < received; i++) {
    descriptor[i] = slReadRAM(ENUM_BUFFER_A + i);
    printHexByte(descriptor[i]);
    Serial.print(i + 1 == received ? '\n' : ' ');
  }

  if (received >= 2) {
    Serial.print("bLength=");
    Serial.print(descriptor[0]);
    Serial.print("  bDescriptorType=0x");
    printHexByte(descriptor[1]);
    Serial.println();
  }
  if (received >= 8) {
    Serial.print("EP0 max packet size (bMaxPacketSize0)=");
    Serial.println(descriptor[7]);
  }

  Serial.println();
  Serial.println("FIRST SETUP + IN DATA STAGES COMPLETED.");
  Serial.println("Control STATUS stage is intentionally not sent in this diagnostic.");
  Serial.println("Run POSEIDON again before another clean ENUM8 attempt.");
}



// ==========================================
// ENUMERATION STEP 2: FULL DEVICE DESCRIPTOR
// + STATUS OUT + SET_ADDRESS
// ==========================================

bool enumTransferSucceeded(const EnumTransferResult &result) {
  return result.started && result.done && result.ack && result.sequenceOK;
}

void enumPrintDescriptor(const uint8_t *descriptor, uint8_t length) {
  Serial.println();
  Serial.println("Device descriptor:");
  for (uint8_t i = 0; i < length; i++) {
    printHexByte(descriptor[i]);
    if (i + 1 == length || i % 8 == 7)
      Serial.println();
    else
      Serial.print(' ');
  }

  if (length >= 2) {
    Serial.print("bLength=");
    Serial.print(descriptor[0]);
    Serial.print("  bDescriptorType=0x");
    printHexByte(descriptor[1]);
    Serial.println();
  }
  if (length >= 8) {
    Serial.print("bMaxPacketSize0=");
    Serial.println(descriptor[7]);
  }
  if (length >= 12) {
    uint16_t vid = (uint16_t)descriptor[8] | ((uint16_t)descriptor[9] << 8);
    uint16_t pid = (uint16_t)descriptor[10] | ((uint16_t)descriptor[11] << 8);
    Serial.print("VID=0x");
    if (vid < 0x1000) Serial.print('0');
    Serial.print(vid, HEX);
    Serial.print("  PID=0x");
    if (pid < 0x1000) Serial.print('0');
    Serial.println(pid, HEX);
  }
}

void poseidonEnum18SetAddress() {
  enumAddress1Ready = false;
  Serial.println();
  Serial.println("========================================");
  Serial.println(" FULL DEVICE DESCRIPTOR + SET_ADDRESS");
  Serial.println("========================================");
  Serial.println("Uses current POSEIDON initialization; no USB reset is issued.");

  if (!poseidonReady) {
    Serial.println("ERROR: Run POSEIDON first and confirm a connected Full-Speed device.");
    return;
  }

  uint8_t ctrl1 = slReadRegister(REG_CTRL1);
  uint8_t intEnable = slReadRegister(REG_INTENABLE);
  uint8_t port = slReadRegister(REG_INTSTATUS);
  Serial.print("CONTROL1=0x"); printHexByte(ctrl1);
  Serial.print("  INTENABLE=0x"); printHexByte(intEnable);
  Serial.print("  PORT/INTSTATUS=0x"); printHexByte(port);
  Serial.println();

  if (!(ctrl1 & 0x01) || !(intEnable & ENUM_INT_USB_A) ||
      (port & ENUM_INT_DETECT) || !(port & 0x80)) {
    Serial.println("ERROR: SOF, USB-A interrupt, or Full-Speed connection state is not valid.");
    Serial.println("Run POSEIDON again before retrying.");
    return;
  }

  // This is a one-shot sequence: it leaves the device at address 1.
  // Require POSEIDON reinitialization before another independent enumeration.
  poseidonReady = false;

  uint8_t descriptor[18] = {0};

  // USB enumeration starts with an 8-byte read so the host learns EP0 MPS
  // before requesting the complete descriptor.
  const uint8_t getDescriptor8[8] = {
    0x80, 0x06, 0x00, 0x01, 0x00, 0x00, 0x08, 0x00
  };

  Serial.println("\n[1] GET_DESCRIPTOR(Device), wLength=8, address=0");
  Serial.println("Waiting 50 ms for USB reset recovery...");
  delay(50);

  EnumTransferResult firstSetup = enumIssueChannelA_with_retry(
    ENUM_PID_SETUP, true, false, ENUM_DEVICE_ADDRESS,
    ENUM_BUFFER_A, 8, getDescriptor8);

  if (!enumTransferSucceeded(firstSetup)) {
    enumPrintTransferResult("SETUP: GET_DESCRIPTOR(Device), wLength=8",
                            firstSetup, ENUM_PID_SETUP, true, false, 8);
    Serial.println("STOP: initial descriptor SETUP failed.");
    return;
  }

  // No Serial output between SETUP ACK and its first DATA packet.
  EnumTransferResult firstIn = enumIssueChannelA_with_retry(
    ENUM_PID_IN, false, true, ENUM_DEVICE_ADDRESS,
    ENUM_BUFFER_A, 8, NULL);

  enumPrintTransferResult("SETUP: GET_DESCRIPTOR(Device), wLength=8",
                          firstSetup, ENUM_PID_SETUP, true, false, 8);
  enumPrintTransferResult("IN: first 8 descriptor bytes, DATA1",
                          firstIn, ENUM_PID_IN, false, true, 8);

  if (!enumTransferSucceeded(firstIn) || firstIn.txLeft > 8) {
    Serial.println("STOP: initial descriptor IN failed or returned an invalid count.");
    return;
  }

  uint8_t firstReceived = (uint8_t)(8 - firstIn.txLeft);
  for (uint8_t i = 0; i < firstReceived; i++)
    descriptor[i] = slReadRAM(ENUM_BUFFER_A + i);

  Serial.print("Initial descriptor bytes received: ");
  Serial.println(firstReceived);
  enumPrintDescriptor(descriptor, firstReceived);

  // Finish this first control read before issuing the full descriptor request.
  EnumTransferResult firstStatus = enumIssueChannelA_with_retry(
    ENUM_PID_OUT, true, true, ENUM_DEVICE_ADDRESS,
    ENUM_BUFFER_A, 0, NULL);
  enumPrintTransferResult("STATUS OUT: initial GET_DESCRIPTOR, DATA1, zero-length",
                          firstStatus, ENUM_PID_OUT, true, true, 0);

  if (!enumTransferSucceeded(firstStatus)) {
    Serial.println("STOP: initial GET_DESCRIPTOR STATUS OUT failed.");
    return;
  }

  if (firstReceived != 8 || descriptor[0] != 18 || descriptor[1] != 0x01) {
    Serial.println("STOP: first 8 descriptor bytes are incomplete or have an unexpected header.");
    Serial.println("SET_ADDRESS was not sent.");
    return;
  }

  uint8_t ep0PacketSize = descriptor[7];
  if (ep0PacketSize != 8 && ep0PacketSize != 16 &&
      ep0PacketSize != 32 && ep0PacketSize != 64) {
    Serial.println("STOP: invalid bMaxPacketSize0; SET_ADDRESS was not sent.");
    return;
  }

  Serial.print("EP0 maximum packet size learned from descriptor: ");
  Serial.println(ep0PacketSize);

  // Request all 18 bytes. Split into legal EP0 packets if MPS is below 18.
  const uint8_t getDescriptor18[8] = {
    0x80, 0x06, 0x00, 0x01, 0x00, 0x00, 0x12, 0x00
  };

  Serial.println("\n[2] GET_DESCRIPTOR(Device), wLength=18, address=0");
  EnumTransferResult fullSetup = enumIssueChannelA_with_retry(
    ENUM_PID_SETUP, true, false, ENUM_DEVICE_ADDRESS,
    ENUM_BUFFER_A, 8, getDescriptor18);

  if (!enumTransferSucceeded(fullSetup)) {
    enumPrintTransferResult("SETUP: GET_DESCRIPTOR(Device), wLength=18",
                            fullSetup, ENUM_PID_SETUP, true, false, 8);
    Serial.println("STOP: full descriptor SETUP failed.");
    return;
  }

  uint8_t totalReceived = 0;
  bool dataToggle = true;  // First DATA packet after SETUP is DATA1.
  bool fullReadOK = true;
  uint8_t packetNumber = 0;

  while (totalReceived < 18) {
    uint8_t remaining = (uint8_t)(18 - totalReceived);
    uint8_t packetLength = (remaining < ep0PacketSize) ? remaining : ep0PacketSize;
    uint8_t packetBuffer = (uint8_t)(ENUM_BUFFER_A + totalReceived);

    // Keep each packet in a distinct SL811 RAM range so earlier bytes survive.
    EnumTransferResult packetResult = enumIssueChannelA_with_retry(
      ENUM_PID_IN, false, dataToggle, ENUM_DEVICE_ADDRESS,
      packetBuffer, packetLength, NULL);

    packetNumber++;
    enumPrintTransferResult("IN: full descriptor packet",
                            packetResult, ENUM_PID_IN, false, dataToggle,
                            packetLength);

    if (!enumTransferSucceeded(packetResult) || packetResult.txLeft > packetLength) {
      Serial.println("STOP: full descriptor IN packet failed.");
      fullReadOK = false;
      break;
    }

    uint8_t packetReceived = (uint8_t)(packetLength - packetResult.txLeft);
    for (uint8_t i = 0; i < packetReceived; i++)
      descriptor[totalReceived + i] = slReadRAM(packetBuffer + i);

    totalReceived = (uint8_t)(totalReceived + packetReceived);
    Serial.print("Descriptor packet ");
    Serial.print(packetNumber);
    Serial.print(": ");
    Serial.print(packetReceived);
    Serial.print(" bytes; total ");
    Serial.print(totalReceived);
    Serial.println(" / 18");

    // A short packet terminates the data stage. Otherwise toggle DATA0/DATA1.
    if (packetReceived < packetLength)
      break;
    dataToggle = !dataToggle;
  }

  if (!fullReadOK)
    return;

  Serial.print("Full descriptor bytes received: ");
  Serial.println(totalReceived);
  enumPrintDescriptor(descriptor, totalReceived);

  // Complete the control read even if the device returned a short descriptor.
  EnumTransferResult fullStatus = enumIssueChannelA_with_retry(
    ENUM_PID_OUT, true, true, ENUM_DEVICE_ADDRESS,
    ENUM_BUFFER_A, 0, NULL);
  enumPrintTransferResult("STATUS OUT: full GET_DESCRIPTOR, DATA1, zero-length",
                          fullStatus, ENUM_PID_OUT, true, true, 0);

  if (!enumTransferSucceeded(fullStatus)) {
    Serial.println("STOP: full GET_DESCRIPTOR STATUS OUT failed; address unchanged.");
    return;
  }

  if (totalReceived != 18 || descriptor[0] != 18 || descriptor[1] != 0x01) {
    Serial.println("STOP: full device descriptor is incomplete or has an unexpected header.");
    Serial.println("SET_ADDRESS was not sent.");
    return;
  }

  // Standard SET_ADDRESS. Both SETUP and its status IN use address zero.
  const uint8_t setAddressPacket[8] = {
    0x00, 0x05, ENUM_NEW_DEVICE_ADDRESS, 0x00,
    0x00, 0x00, 0x00, 0x00
  };

  Serial.println("\n[3] SET_ADDRESS request: 0 -> 1");
  EnumTransferResult addressSetup = enumIssueChannelA_with_retry(
    ENUM_PID_SETUP, true, false, ENUM_DEVICE_ADDRESS,
    ENUM_BUFFER_A, 8, setAddressPacket);

  if (!enumTransferSucceeded(addressSetup)) {
    enumPrintTransferResult("SETUP: SET_ADDRESS(1), DATA0, address=0",
                            addressSetup, ENUM_PID_SETUP, true, false, 8);
    Serial.println("STOP: SET_ADDRESS SETUP failed.");
    return;
  }

  // SET_ADDRESS has no DATA stage; its status stage is IN, DATA1, zero-length.
  EnumTransferResult addressStatus = enumIssueChannelA_with_retry(
    ENUM_PID_IN, false, true, ENUM_DEVICE_ADDRESS,
    ENUM_BUFFER_A, 0, NULL);

  enumPrintTransferResult("SETUP: SET_ADDRESS(1), DATA0, address=0",
                          addressSetup, ENUM_PID_SETUP, true, false, 8);
  enumPrintTransferResult("STATUS IN: SET_ADDRESS, DATA1, zero-length, address=0",
                          addressStatus, ENUM_PID_IN, false, true, 0);

  if (!enumTransferSucceeded(addressStatus) || addressStatus.txLeft != 0) {
    Serial.println("STOP: SET_ADDRESS status stage failed; do not use address 1.");
    return;
  }

  // The device changes address after the status handshake. Wait before use.
  delay(2);
  Serial.println("SET_ADDRESS status stage ACKed. Waiting 2 ms before using address 1.");

  // Verify address 1 by fetching the first 8 bytes of the same descriptor.
  const uint8_t verifyDescriptor8[8] = {
    0x80, 0x06, 0x00, 0x01, 0x00, 0x00, 0x08, 0x00
  };

  Serial.println("\n[4] Verify new device address with GET_DESCRIPTOR(8), address=1");
  EnumTransferResult verifySetup = enumIssueChannelA_with_retry(
    ENUM_PID_SETUP, true, false, ENUM_NEW_DEVICE_ADDRESS,
    ENUM_BUFFER_A, 8, verifyDescriptor8);

  if (!enumTransferSucceeded(verifySetup)) {
    enumPrintTransferResult("SETUP: verify address 1, GET_DESCRIPTOR(8)",
                            verifySetup, ENUM_PID_SETUP, true, false, 8);
    Serial.println("WARNING: SET_ADDRESS was ACKed, but address 1 did not answer the verification SETUP.");
    return;
  }

  EnumTransferResult verifyIn = enumIssueChannelA_with_retry(
    ENUM_PID_IN, false, true, ENUM_NEW_DEVICE_ADDRESS,
    ENUM_BUFFER_A, 8, NULL);

  enumPrintTransferResult("SETUP: verify address 1, GET_DESCRIPTOR(8)",
                          verifySetup, ENUM_PID_SETUP, true, false, 8);
  enumPrintTransferResult("IN: verify descriptor at address 1, DATA1",
                          verifyIn, ENUM_PID_IN, false, true, 8);

  if (!enumTransferSucceeded(verifyIn) || verifyIn.txLeft > 8) {
    Serial.println("WARNING: address 1 SETUP succeeded, but descriptor IN verification failed.");
    return;
  }

  uint8_t verifyReceived = (uint8_t)(8 - verifyIn.txLeft);
  uint8_t verifyBytes[8] = {0};
  for (uint8_t i = 0; i < verifyReceived; i++)
    verifyBytes[i] = slReadRAM(ENUM_BUFFER_A + i);

  bool descriptorMatches = (verifyReceived == 8);
  for (uint8_t i = 0; i < verifyReceived && i < 8; i++) {
    if (verifyBytes[i] != descriptor[i]) descriptorMatches = false;
  }

  Serial.print("Address-1 verification bytes received: ");
  Serial.println(verifyReceived);
  Serial.print("First 8 descriptor bytes match address-0 read: ");
  Serial.println(descriptorMatches ? "YES" : "NO");

  EnumTransferResult verifyStatus = enumIssueChannelA_with_retry(
    ENUM_PID_OUT, true, true, ENUM_NEW_DEVICE_ADDRESS,
    ENUM_BUFFER_A, 0, NULL);
  enumPrintTransferResult("STATUS OUT: address-1 verification, DATA1, zero-length",
                          verifyStatus, ENUM_PID_OUT, true, true, 0);

  if (!enumTransferSucceeded(verifyStatus)) {
    Serial.println("WARNING: address 1 answered, but verification STATUS OUT failed.");
    return;
  }

  if (descriptorMatches) {
    memcpy(enumStoredDeviceDescriptor, descriptor, sizeof(enumStoredDeviceDescriptor));
    enumStoredEp0PacketSize = ep0PacketSize;
    enumAddress1Ready = true;

    Serial.println();
    Serial.println("========================================");
    Serial.println(" ENUMERATION CHECKPOINT PASSED");
    Serial.println(" 8-byte initial descriptor read completed.");
    Serial.println(" Full 18-byte descriptor received.");
    Serial.println(" Both GET_DESCRIPTOR STATUS OUT stages completed.");
    Serial.println(" SET_ADDRESS status IN completed at address 0.");
    Serial.println(" Device answered at the new address 1.");
    Serial.println(" Address 1 retained for IDENTIFY command.");
    Serial.println("========================================");
  } else {
    Serial.println("Address 1 responded, but descriptor data did not match the original read.");
  }
}


// ==========================================
// IDENTIFY: USB STRINGS + CONFIGURATION TREE
// ==========================================

#define IDENTIFY_MAX_CONFIG_BYTES 512
#define IDENTIFY_MAX_EXTRA_STRINGS 16

const char *enumUsbClassName(uint8_t cls) {
  switch (cls) {
    case 0x00: return "Per-interface (defined by interface descriptors)";
    case 0x01: return "Audio";
    case 0x02: return "Communications / CDC Control";
    case 0x03: return "Human Interface Device (HID)";
    case 0x05: return "Physical";
    case 0x06: return "Still Image";
    case 0x07: return "Printer";
    case 0x08: return "Mass Storage";
    case 0x09: return "Hub";
    case 0x0A: return "CDC Data";
    case 0x0B: return "Smart Card";
    case 0x0D: return "Content Security";
    case 0x0E: return "Video";
    case 0x0F: return "Personal Healthcare";
    case 0x10: return "Audio/Video";
    case 0xDC: return "Diagnostic";
    case 0xE0: return "Wireless Controller";
    case 0xEF: return "Miscellaneous";
    case 0xFE: return "Application Specific";
    case 0xFF: return "Vendor Specific";
    default:   return "Reserved / class-specific";
  }
}

const char *enumEndpointTypeName(uint8_t attributes) {
  switch (attributes & 0x03) {
    case 0: return "Control";
    case 1: return "Isochronous";
    case 2: return "Bulk";
    case 3: return "Interrupt";
    default: return "Unknown";
  }
}

void enumAddStringIndex(uint8_t *indices, uint8_t &count, uint8_t value) {
  if (value == 0 || count >= IDENTIFY_MAX_EXTRA_STRINGS)
    return;
  for (uint8_t i = 0; i < count; i++) {
    if (indices[i] == value)
      return;
  }
  indices[count++] = value;
}

// Performs one complete standard control-read GET_DESCRIPTOR transaction.
// The SL811HS RAM buffer is reused for every IN packet; bytes are copied to
// the Arduino buffer immediately after each completed packet.
bool enumControlGetDescriptor(uint8_t deviceAddress,
                              uint8_t ep0PacketSize,
                              uint8_t descriptorType,
                              uint8_t descriptorIndex,
                              uint16_t languageId,
                              uint16_t requestedLength,
                              uint8_t *destination,
                              uint16_t destinationCapacity,
                              uint16_t &actualLength,
                              const char *description) {
  actualLength = 0;

  if (destination == NULL || requestedLength == 0 ||
      requestedLength > destinationCapacity ||
      (ep0PacketSize != 8 && ep0PacketSize != 16 &&
       ep0PacketSize != 32 && ep0PacketSize != 64)) {
    Serial.println("IDENTIFY ERROR: invalid descriptor buffer length or EP0 packet size.");
    return false;
  }

  uint8_t setupPacket[8] = {
    0x80, 0x06,
    descriptorIndex, descriptorType,
    (uint8_t)(languageId & 0xFF),
    (uint8_t)(languageId >> 8),
    (uint8_t)(requestedLength & 0xFF),
    (uint8_t)(requestedLength >> 8)
  };

  Serial.println();
  Serial.print("IDENTIFY request: ");
  Serial.println(description);
  Serial.print("  GET_DESCRIPTOR type=0x");
  printHexByte(descriptorType);
  Serial.print(" index=");
  Serial.print(descriptorIndex);
  Serial.print(" language=0x");
  printHexByte((uint8_t)(languageId >> 8));
  printHexByte((uint8_t)languageId);
  Serial.print(" wLength=");
  Serial.println(requestedLength);

  EnumTransferResult setupResult = enumIssueChannelA_with_retry(
    ENUM_PID_SETUP, true, false, deviceAddress,
    ENUM_BUFFER_A, 8, setupPacket);
  enumPrintTransferResult("GET_DESCRIPTOR SETUP, DATA0", setupResult,
                          ENUM_PID_SETUP, true, false, 8);
  if (!enumTransferSucceeded(setupResult)) {
    Serial.println("IDENTIFY: descriptor SETUP failed.");
    return false;
  }

  uint16_t receivedTotal = 0;
  bool dataToggle = true; // First data packet after SETUP is DATA1.
  uint8_t packetNumber = 0;

  while (receivedTotal < requestedLength) {
    uint16_t remaining = requestedLength - receivedTotal;
    uint8_t packetLength = (remaining < ep0PacketSize)
                         ? (uint8_t)remaining : ep0PacketSize;

    EnumTransferResult inResult = enumIssueChannelA_with_retry(
      ENUM_PID_IN, false, dataToggle, deviceAddress,
      ENUM_BUFFER_A, packetLength, NULL);

    packetNumber++;
    enumPrintTransferResult("GET_DESCRIPTOR IN data packet", inResult,
                            ENUM_PID_IN, false, dataToggle, packetLength);

    if (!enumTransferSucceeded(inResult) || inResult.txLeft > packetLength) {
      Serial.println("IDENTIFY: descriptor IN packet failed.");
      return false;
    }

    uint8_t packetReceived = (uint8_t)(packetLength - inResult.txLeft);
    for (uint8_t i = 0; i < packetReceived; i++)
      destination[receivedTotal + i] = slReadRAM(ENUM_BUFFER_A + i);

    receivedTotal += packetReceived;
    Serial.print("  Packet ");
    Serial.print(packetNumber);
    Serial.print(": ");
    Serial.print(packetReceived);
    Serial.print(" bytes; total ");
    Serial.print(receivedTotal);
    Serial.print(" / ");
    Serial.println(requestedLength);

    // A short packet terminates the control data stage. If the requested
    // length was exhausted, the control data stage is complete as well.
    if (packetReceived < packetLength || receivedTotal >= requestedLength)
      break;

    dataToggle = !dataToggle;
  }

  EnumTransferResult statusResult = enumIssueChannelA_with_retry(
    ENUM_PID_OUT, true, true, deviceAddress,
    ENUM_BUFFER_A, 0, NULL);
  enumPrintTransferResult("GET_DESCRIPTOR STATUS OUT, DATA1, zero-length",
                          statusResult, ENUM_PID_OUT, true, true, 0);

  if (!enumTransferSucceeded(statusResult)) {
    Serial.println("IDENTIFY: descriptor STATUS OUT failed.");
    return false;
  }

  actualLength = receivedTotal;
  Serial.print("IDENTIFY descriptor read complete: ");
  Serial.print(actualLength);
  Serial.println(" bytes.");
  return true;
}

void enumPrintUsbString(const char *label,
                        const uint8_t *descriptor,
                        uint16_t receivedLength) {
  Serial.print(label);
  Serial.print(": ");

  if (receivedLength < 2 || descriptor[1] != 0x03) {
    Serial.println("<missing or invalid STRING descriptor>");
    return;
  }

  uint8_t descriptorLength = descriptor[0];
  if (descriptorLength < 2 || descriptorLength > receivedLength) {
    Serial.print("<truncated STRING descriptor; bLength=");
    Serial.print(descriptorLength);
    Serial.print(", received=");
    Serial.print(receivedLength);
    Serial.println(">");
    return;
  }

  for (uint8_t i = 2; i + 1 < descriptorLength; i += 2) {
    uint16_t codePoint = (uint16_t)descriptor[i] |
                         ((uint16_t)descriptor[i + 1] << 8);
    if (codePoint >= 0x20 && codePoint <= 0x7E) {
      Serial.write((uint8_t)codePoint);
    } else if (codePoint == 0x09) {
      Serial.write(' ');
    } else if (codePoint != 0) {
      Serial.print("\\u");
      printHexByte((uint8_t)(codePoint >> 8));
      printHexByte((uint8_t)codePoint);
    }
  }
  Serial.println();
}

bool enumReadAndPrintString(uint8_t stringIndex,
                            uint16_t languageId,
                            uint8_t ep0PacketSize,
                            const char *label) {
  if (stringIndex == 0) {
    Serial.print(label);
    Serial.println(": <not provided (index 0)>");
    return true;
  }

  uint8_t stringDescriptor[255] = {0};
  uint16_t received = 0;
  if (!enumControlGetDescriptor(ENUM_NEW_DEVICE_ADDRESS, ep0PacketSize,
                                0x03, stringIndex, languageId,
                                255, stringDescriptor,
                                sizeof(stringDescriptor), received, label)) {
    Serial.print(label);
    Serial.println(": <request failed>");
    return false;
  }

  enumPrintUsbString(label, stringDescriptor, received);
  return true;
}

void enumPrintConfigurationTree(const uint8_t *data,
                                uint16_t length,
                                uint8_t *extraStringIndices,
                                uint8_t &extraStringCount) {
  Serial.println();
  Serial.println("CONFIGURATION DESCRIPTOR TREE");
  Serial.println("-----------------------------");

  uint16_t offset = 0;
  uint8_t currentInterface = 0xFF;

  while (offset + 2 <= length) {
    uint8_t descriptorLength = data[offset];
    uint8_t descriptorType = data[offset + 1];

    if (descriptorLength < 2 || offset + descriptorLength > length) {
      Serial.print("Malformed/truncated descriptor at offset 0x");
      printHexByte((uint8_t)offset);
      Serial.println("; stopping parser.");
      break;
    }

    if (descriptorType == 0x02 && descriptorLength >= 9) {
      Serial.println("Configuration:");
      Serial.print("  bNumInterfaces="); Serial.println(data[offset + 4]);
      Serial.print("  bConfigurationValue="); Serial.println(data[offset + 5]);
      Serial.print("  iConfiguration="); Serial.println(data[offset + 6]);
      Serial.print("  bmAttributes=0x"); printHexByte(data[offset + 7]);
      Serial.print("  MaxPower=");
      Serial.print((uint16_t)data[offset + 8] * 2);
      Serial.println(" mA");
      enumAddStringIndex(extraStringIndices, extraStringCount, data[offset + 6]);
    }
    else if (descriptorType == 0x04 && descriptorLength >= 9) {
      currentInterface = data[offset + 2];
      uint8_t cls = data[offset + 5];
      uint8_t sub = data[offset + 6];
      uint8_t proto = data[offset + 7];

      Serial.println("Interface:");
      Serial.print("  Number="); Serial.print(currentInterface);
      Serial.print("  Alternate="); Serial.println(data[offset + 3]);
      Serial.print("  Endpoints="); Serial.println(data[offset + 4]);
      Serial.print("  Class=0x"); printHexByte(cls);
      Serial.print(" (");
      Serial.print(enumUsbClassName(cls));
      Serial.println(")");
      Serial.print("  SubClass=0x"); printHexByte(sub);
      Serial.print("  Protocol=0x"); printHexByte(proto);
      Serial.print("  iInterface="); Serial.println(data[offset + 8]);

      if (cls == 0x03 && sub == 0x01) {
        if (proto == 0x01) Serial.println("  HID boot protocol: KEYBOARD");
        else if (proto == 0x02) Serial.println("  HID boot protocol: MOUSE");
      }
      if (cls == 0x08) {
        if (sub == 0x06) Serial.println("  Mass Storage subclass: SCSI transparent");
        if (proto == 0x50) Serial.println("  Mass Storage protocol: Bulk-Only Transport (BOT)");
      }
      if (cls == 0x07) Serial.println("  Printer interface detected");

      enumAddStringIndex(extraStringIndices, extraStringCount, data[offset + 8]);
    }
    else if (descriptorType == 0x05 && descriptorLength >= 7) {
      uint8_t address = data[offset + 2];
      uint8_t attributes = data[offset + 3];
      uint16_t maxPacket = (uint16_t)data[offset + 4] |
                           ((uint16_t)data[offset + 5] << 8);
      maxPacket &= 0x07FF;

      Serial.print("Endpoint (interface ");
      if (currentInterface == 0xFF) Serial.print('?');
      else Serial.print(currentInterface);
      Serial.println("):");
      Serial.print("  Address=0x"); printHexByte(address);
      Serial.print("  EP="); Serial.print(address & 0x0F);
      Serial.print("  Direction="); Serial.println((address & 0x80) ? "IN" : "OUT");
      Serial.print("  Transfer="); Serial.println(enumEndpointTypeName(attributes));
      Serial.print("  MaxPacketSize="); Serial.println(maxPacket);
      Serial.print("  Interval="); Serial.println(data[offset + 6]);
    }
    else if (descriptorType == 0x21 && descriptorLength >= 6) {
      Serial.print("HID descriptor (interface ");
      if (currentInterface == 0xFF) Serial.print('?');
      else Serial.print(currentInterface);
      Serial.println("):");
      Serial.print("  HID version=0x");
      printHexByte(data[offset + 3]);
      printHexByte(data[offset + 2]);
      Serial.print("  Country code="); Serial.println(data[offset + 4]);
      Serial.print("  subordinate descriptors="); Serial.println(data[offset + 5]);
      if (descriptorLength >= 9) {
        Serial.print("  Report descriptor length=");
        Serial.println((uint16_t)data[offset + 7] |
                       ((uint16_t)data[offset + 8] << 8));
      }
    }
    else if (descriptorType == 0x0B && descriptorLength >= 8) {
      Serial.println("Interface Association Descriptor (IAD)");
      Serial.print("  First interface="); Serial.println(data[offset + 2]);
      Serial.print("  Interface count="); Serial.println(data[offset + 3]);
      Serial.print("  Function class=0x"); printHexByte(data[offset + 4]);
      Serial.print("  SubClass=0x"); printHexByte(data[offset + 5]);
      Serial.print("  Protocol=0x"); printHexByte(data[offset + 6]);
    }

    offset += descriptorLength;
  }
}

void enumDumpBytes(const char *label, const uint8_t *data, uint16_t length) {
  Serial.println();
  Serial.println(label);
  for (uint16_t i = 0; i < length; i++) {
    printHexByte(data[i]);
    if ((i & 0x0F) == 0x0F || i + 1 == length)
      Serial.println();
    else
      Serial.print(' ');
  }
}

void poseidonIdentify() {
  Serial.println();
  Serial.println("========================================");
  Serial.println(" IDENTIFY USB DEVICE AT ADDRESS 1");
  Serial.println("========================================");
  Serial.println("Read-only descriptor inspection; SET_CONFIGURATION is not sent.");

  if (!enumAddress1Ready) {
    Serial.println("ERROR: No verified device at address 1.");
    Serial.println("Run POSEIDON, then ENUM18 successfully, then IDENTIFY.");
    return;
  }

  uint8_t ctrl1 = slReadRegister(REG_CTRL1);
  uint8_t port = slReadRegister(REG_INTSTATUS);
  if (!(ctrl1 & 0x01) || (port & ENUM_INT_DETECT) || !(port & 0x80)) {
    Serial.println("ERROR: USB bus is no longer in the expected connected Full-Speed state.");
    Serial.println("Run POSEIDON and ENUM18 again.");
    enumAddress1Ready = false;
    return;
  }

  const uint8_t *device = enumStoredDeviceDescriptor;
  uint16_t vid = (uint16_t)device[8] | ((uint16_t)device[9] << 8);
  uint16_t pid = (uint16_t)device[10] | ((uint16_t)device[11] << 8);
  uint16_t bcdDevice = (uint16_t)device[12] | ((uint16_t)device[13] << 8);

  Serial.println("\nSTORED DEVICE DESCRIPTOR (from successful ENUM18)");
  Serial.print("VID=0x"); printHexByte((uint8_t)(vid >> 8)); printHexByte((uint8_t)vid);
  Serial.print("  PID=0x"); printHexByte((uint8_t)(pid >> 8)); printHexByte((uint8_t)pid);
  Serial.print("  bcdDevice=0x"); printHexByte((uint8_t)(bcdDevice >> 8)); printHexByte((uint8_t)bcdDevice);
  Serial.print("\nDevice class=0x"); printHexByte(device[4]);
  Serial.print("  SubClass=0x"); printHexByte(device[5]);
  Serial.print("  Protocol=0x"); printHexByte(device[6]);
  Serial.print("\nEP0 max packet="); Serial.println(enumStoredEp0PacketSize);
  Serial.print("Configurations="); Serial.println(device[17]);

  // String descriptor zero supplies supported LANGIDs. Do not guess a language.
  uint8_t languageDescriptor[64] = {0};
  uint16_t languageLength = 0;
  uint16_t languageId = 0;
  bool haveLanguage = enumControlGetDescriptor(
    ENUM_NEW_DEVICE_ADDRESS, enumStoredEp0PacketSize,
    0x03, 0x00, 0x0000, 64,
    languageDescriptor, sizeof(languageDescriptor), languageLength,
    "String descriptor 0 (supported LANGIDs)");

  if (haveLanguage && languageLength >= 4 &&
      languageDescriptor[1] == 0x03 &&
      languageDescriptor[0] >= 4 &&
      languageDescriptor[0] <= languageLength) {
    Serial.println("Supported LANGIDs:");
    for (uint8_t i = 2; i + 1 < languageDescriptor[0]; i += 2) {
      uint16_t lang = (uint16_t)languageDescriptor[i] |
                      ((uint16_t)languageDescriptor[i + 1] << 8);
      Serial.print("  0x");
      printHexByte((uint8_t)(lang >> 8));
      printHexByte((uint8_t)lang);
      if (languageId == 0) languageId = lang;
      Serial.println();
    }
  } else {
    Serial.println("WARNING: no valid LANGID descriptor; string requests will be skipped.");
  }

  if (languageId != 0) {
    Serial.print("Using LANGID 0x");
    printHexByte((uint8_t)(languageId >> 8));
    printHexByte((uint8_t)languageId);
    Serial.println();

    enumReadAndPrintString(device[14], languageId, enumStoredEp0PacketSize,
                           "Manufacturer");
    enumReadAndPrintString(device[15], languageId, enumStoredEp0PacketSize,
                           "Product");
    enumReadAndPrintString(device[16], languageId, enumStoredEp0PacketSize,
                           "Serial number");
  } else {
    Serial.println("Manufacturer/Product/Serial strings not queried.");
  }

  uint8_t configurationCount = device[17];
  if (configurationCount == 0) {
    Serial.println("Device reports zero configurations.");
    return;
  }

  if (configurationCount > 4) {
    Serial.println("NOTE: device reports more than four configurations; inspecting first four only.");
    configurationCount = 4;
  }

  for (uint8_t configIndex = 0; configIndex < configurationCount; configIndex++) {
    Serial.println();
    Serial.println("========================================");
    Serial.print(" CONFIGURATION INDEX ");
    Serial.println(configIndex);
    Serial.println("========================================");

    uint8_t configHeader[9] = {0};
    uint16_t headerReceived = 0;
    if (!enumControlGetDescriptor(ENUM_NEW_DEVICE_ADDRESS,
                                  enumStoredEp0PacketSize,
                                  0x02, configIndex, 0, 9,
                                  configHeader, sizeof(configHeader),
                                  headerReceived, "Configuration descriptor header (9 bytes)")) {
      Serial.println("WARNING: could not read configuration header; moving on.");
      continue;
    }

    if (headerReceived < 9 || configHeader[0] < 9 || configHeader[1] != 0x02) {
      Serial.println("WARNING: malformed configuration descriptor header.");
      continue;
    }

    uint16_t totalLength = (uint16_t)configHeader[2] |
                           ((uint16_t)configHeader[3] << 8);
    Serial.print("Configuration wTotalLength=");
    Serial.println(totalLength);

    if (totalLength < 9) {
      Serial.println("WARNING: invalid wTotalLength (< 9).");
      continue;
    }

    uint16_t requestLength = totalLength;
    if (requestLength > IDENTIFY_MAX_CONFIG_BYTES) {
      requestLength = IDENTIFY_MAX_CONFIG_BYTES;
      Serial.println("WARNING: configuration exceeds 512 bytes; reading first 512 only.");
    }

    uint8_t configData[IDENTIFY_MAX_CONFIG_BYTES] = {0};
    uint16_t configReceived = 0;
    if (!enumControlGetDescriptor(ENUM_NEW_DEVICE_ADDRESS,
                                  enumStoredEp0PacketSize,
                                  0x02, configIndex, 0, requestLength,
                                  configData, sizeof(configData),
                                  configReceived, "Full configuration descriptor")) {
      Serial.println("WARNING: full configuration descriptor read failed.");
      continue;
    }

    Serial.print("Configuration bytes received: ");
    Serial.print(configReceived);
    Serial.print(" / ");
    Serial.println(totalLength);
    enumDumpBytes("Raw configuration descriptor bytes:", configData, configReceived);

    uint8_t extraStringIndices[IDENTIFY_MAX_EXTRA_STRINGS] = {0};
    uint8_t extraStringCount = 0;
    enumPrintConfigurationTree(configData, configReceived,
                               extraStringIndices, extraStringCount);

    if (languageId != 0 && extraStringCount > 0) {
      Serial.println();
      Serial.println("CONFIGURATION / INTERFACE STRINGS");
      for (uint8_t i = 0; i < extraStringCount; i++) {
        char label[32];
        snprintf(label, sizeof(label), "String index %u", (unsigned int)extraStringIndices[i]);
        enumReadAndPrintString(extraStringIndices[i], languageId,
                               enumStoredEp0PacketSize, label);
      }
    }

    if (configReceived < totalLength)
      Serial.println("WARNING: configuration descriptor is incomplete; parser output is partial.");
  }

  Serial.println();
  Serial.println("========================================");
  Serial.println(" IDENTIFY FINISHED");
  Serial.println(" No SET_CONFIGURATION or class-specific request was sent.");
  Serial.println(" Device remains at address 1.");
  Serial.println("========================================");
}

// ==========================================
// HID BOOT MOUSE - INTERRUPT IN VALIDATION
// ==========================================
// Target identified from the Trust/YICHIP receiver configuration:
// Interface 1, HID Boot Mouse, Interrupt IN endpoint 0x82,
// max packet 64 bytes, bInterval 2 ms.
// The test configures the device, selects Boot Protocol, then polls once
// per advertised interval. NAK is expected when there is no new report and
// is NOT retried immediately; only a successful ACK advances DATA toggle.

#define HID_MOUSE_INTERFACE       1
#define HID_MOUSE_ENDPOINT        2
#define HID_MOUSE_ENDPOINT_ADDR   0x82
#define HID_MOUSE_MAX_PACKET      64
#define HID_MOUSE_INTERVAL_MS     2
#define HID_MOUSE_TEST_DURATION_MS 12000UL
#define HID_MOUSE_MAX_CAPTURED    48

bool enumControlNoData(uint8_t requestType,
                       uint8_t request,
                       uint16_t value,
                       uint16_t index,
                       const char *description) {
  uint8_t setupPacket[8] = {
    requestType,
    request,
    (uint8_t)(value & 0xFF),
    (uint8_t)(value >> 8),
    (uint8_t)(index & 0xFF),
    (uint8_t)(index >> 8),
    0,
    0
  };

  Serial.println();
  Serial.print("CONTROL REQUEST: ");
  Serial.println(description);

  EnumTransferResult setupResult = enumIssueChannelA_with_retry(
    ENUM_PID_SETUP, true, false, ENUM_NEW_DEVICE_ADDRESS,
    ENUM_BUFFER_A, 8, setupPacket);
  enumPrintTransferResult("CONTROL SETUP, DATA0", setupResult,
                          ENUM_PID_SETUP, true, false, 8);
  if (!enumTransferSucceeded(setupResult)) {
    Serial.println("Control request SETUP failed.");
    return false;
  }

  // A no-data control OUT request is completed by a zero-length DATA1 IN.
  EnumTransferResult statusResult = enumIssueChannelA_with_retry(
    ENUM_PID_IN, false, true, ENUM_NEW_DEVICE_ADDRESS,
    ENUM_BUFFER_A, 0, NULL);
  enumPrintTransferResult("CONTROL STATUS IN, DATA1, zero-length",
                          statusResult, ENUM_PID_IN, false, true, 0);
  if (!enumTransferSucceeded(statusResult) || statusResult.txLeft != 0) {
    Serial.println("Control request STATUS IN failed.");
    return false;
  }

  Serial.println("Control request completed.");
  return true;
}

// Perform a complete HID/standard control IN request on EP0. The helper
// supports a short single-packet read (the diagnostics below use 1 and 3 B),
// but also handles multiple EP0 packets up to destinationCapacity.
bool enumControlIn(uint8_t requestType,
                   uint8_t request,
                   uint16_t value,
                   uint16_t index,
                   uint16_t requestedLength,
                   uint8_t *destination,
                   uint16_t destinationCapacity,
                   uint16_t &actualLength,
                   const char *description) {
  actualLength = 0;

  if (destination == NULL || requestedLength == 0 ||
      requestedLength > destinationCapacity ||
      (enumStoredEp0PacketSize != 8 && enumStoredEp0PacketSize != 16 &&
       enumStoredEp0PacketSize != 32 && enumStoredEp0PacketSize != 64)) {
    Serial.println("CONTROL IN ERROR: invalid request length, buffer, or EP0 packet size.");
    return false;
  }

  uint8_t setupPacket[8] = {
    requestType,
    request,
    (uint8_t)(value & 0xFF),
    (uint8_t)(value >> 8),
    (uint8_t)(index & 0xFF),
    (uint8_t)(index >> 8),
    (uint8_t)(requestedLength & 0xFF),
    (uint8_t)(requestedLength >> 8)
  };

  Serial.println();
  Serial.print("CONTROL IN REQUEST: ");
  Serial.println(description);
  Serial.print("  bmRequestType=0x"); printHexByte(requestType);
  Serial.print(" bRequest=0x"); printHexByte(request);
  Serial.print(" wValue=0x"); printHexByte((uint8_t)(value >> 8));
  printHexByte((uint8_t)value);
  Serial.print(" wIndex=0x"); printHexByte((uint8_t)(index >> 8));
  printHexByte((uint8_t)index);
  Serial.print(" wLength="); Serial.println(requestedLength);

  EnumTransferResult setupResult = enumIssueChannelA_with_retry(
    ENUM_PID_SETUP, true, false, ENUM_NEW_DEVICE_ADDRESS,
    ENUM_BUFFER_A, 8, setupPacket);
  enumPrintTransferResult("CONTROL IN SETUP, DATA0", setupResult,
                          ENUM_PID_SETUP, true, false, 8);
  if (!enumTransferSucceeded(setupResult)) {
    Serial.println("CONTROL IN: SETUP stage failed.");
    return false;
  }

  uint16_t receivedTotal = 0;
  bool dataToggle = true; // First control data packet after SETUP is DATA1.
  uint8_t packetNumber = 0;

  while (receivedTotal < requestedLength) {
    uint16_t remaining = requestedLength - receivedTotal;
    uint8_t packetLength = (remaining < enumStoredEp0PacketSize)
                         ? (uint8_t)remaining : enumStoredEp0PacketSize;

    EnumTransferResult inResult = enumIssueChannelA_with_retry(
      ENUM_PID_IN, false, dataToggle, ENUM_NEW_DEVICE_ADDRESS,
      ENUM_BUFFER_A, packetLength, NULL);
    packetNumber++;
    enumPrintTransferResult("CONTROL IN DATA", inResult,
                            ENUM_PID_IN, false, dataToggle, packetLength);

    if (!enumTransferSucceeded(inResult) || inResult.txLeft > packetLength) {
      Serial.println("CONTROL IN: data stage failed.");
      return false;
    }

    uint8_t packetReceived = (uint8_t)(packetLength - inResult.txLeft);
    for (uint8_t i = 0; i < packetReceived; i++)
      destination[receivedTotal + i] = slReadRAM(ENUM_BUFFER_A + i);

    receivedTotal += packetReceived;
    Serial.print("  Packet "); Serial.print(packetNumber);
    Serial.print(": "); Serial.print(packetReceived);
    Serial.print(" bytes; total "); Serial.print(receivedTotal);
    Serial.print(" / "); Serial.println(requestedLength);

    // A short packet terminates the control data stage.
    if (packetReceived < packetLength)
      break;

    dataToggle = !dataToggle;
  }

  // A control IN transfer completes with a zero-length STATUS OUT DATA1.
  EnumTransferResult statusResult = enumIssueChannelA_with_retry(
    ENUM_PID_OUT, true, true, ENUM_NEW_DEVICE_ADDRESS,
    ENUM_BUFFER_A, 0, NULL);
  enumPrintTransferResult("CONTROL IN STATUS OUT, DATA1, zero-length",
                          statusResult, ENUM_PID_OUT, true, true, 0);
  if (!enumTransferSucceeded(statusResult)) {
    Serial.println("CONTROL IN: STATUS OUT stage failed.");
    return false;
  }

  actualLength = receivedTotal;
  Serial.print("CONTROL IN completed: ");
  Serial.print(actualLength);
  Serial.println(" data bytes received.");
  return true;
}

// ==========================================
// USB MASS STORAGE - GENERIC BULK-ONLY TRANSPORT (BOT)
// ==========================================
// SCSI INQUIRY milestone. No VID/PID matching is used. The Mass Storage
// interface, configuration, bulk endpoints and packet sizes are discovered
// from the active device's descriptors after ENUM18.

#define MSD_MAX_CONFIG_BYTES       512
#define MSD_BULK_TIMEOUT_MS        1500
#define MSD_CSW_TIMEOUT_MS          10000UL  // Allow slow readers/media to finish a SCSI command.
#define MSD_INQUIRY_LENGTH         36
#define MSD_CBW_LENGTH             31
#define MSD_CSW_LENGTH             13
#define MSD_CBW_SIGNATURE          0x43425355UL
#define MSD_CSW_SIGNATURE          0x53425355UL
#define MSD_INQUIRY_TAG            0x31384C53UL  // little-endian bytes spell "SL81"

struct MsdBotInterface {
  bool found;
  uint8_t configurationIndex;
  uint8_t configurationValue;
  uint8_t interfaceNumber;
  uint8_t alternateSetting;
  uint8_t bulkOutAddress;
  uint8_t bulkInAddress;
  uint8_t bulkOutMaxPacket;
  uint8_t bulkInMaxPacket;
};

void msdPrintLe32(const char *label, const uint8_t *p) {
  uint32_t value = (uint32_t)p[0] |
                   ((uint32_t)p[1] << 8) |
                   ((uint32_t)p[2] << 16) |
                   ((uint32_t)p[3] << 24);
  Serial.print(label);
  Serial.print("0x");
  for (int8_t shift = 24; shift >= 0; shift -= 8)
    printHexByte((uint8_t)(value >> shift));
  Serial.println();
}

void msdPrintAsciiField(const char *label,
                        const uint8_t *data,
                        uint8_t offset,
                        uint8_t length) {
  Serial.print(label);
  Serial.print('"');
  for (uint8_t i = 0; i < length; i++) {
    uint8_t c = data[offset + i];
    Serial.print((c >= 0x20 && c <= 0x7E) ? (char)c : '.');
  }
  Serial.println('"');
}

bool msdValidFullSpeedBulkPacketSize(uint16_t size) {
  // SL811HS is being used as a Full-Speed host. Full-Speed bulk endpoints
  // advertise one of these packet sizes; high-bandwidth bits are not valid.
  return (size == 8 || size == 16 || size == 32 || size == 64);
}

bool msdFindBotInterface(MsdBotInterface &selection) {
  memset(&selection, 0, sizeof(selection));
  selection.found = false;

  if (!enumAddress1Ready) {
    Serial.println("MSD discovery: no verified USB device at address 1.");
    return false;
  }

  uint8_t configurationCount = enumStoredDeviceDescriptor[17];
  if (configurationCount == 0) {
    Serial.println("MSD discovery: device reports no configurations.");
    return false;
  }

  Serial.println();
  Serial.println("[MSD] Scanning configuration descriptors for:");
  Serial.println("      Class 08 / SubClass 06 / Protocol 50 (SCSI + BOT)");

  for (uint16_t configIndex = 0; configIndex < configurationCount; configIndex++) {
    uint8_t configHeader[9] = {0};
    uint16_t headerReceived = 0;

    if (!enumControlGetDescriptor(ENUM_NEW_DEVICE_ADDRESS,
                                  enumStoredEp0PacketSize,
                                  0x02, (uint8_t)configIndex, 0, 9,
                                  configHeader, sizeof(configHeader),
                                  headerReceived, "MSD configuration header")) {
      Serial.print("[MSD] Cannot read configuration index ");
      Serial.println(configIndex);
      continue;
    }

    if (headerReceived < 9 || configHeader[0] < 9 || configHeader[1] != 0x02) {
      Serial.print("[MSD] Invalid configuration header at index ");
      Serial.println(configIndex);
      continue;
    }

    uint16_t totalLength = (uint16_t)configHeader[2] |
                           ((uint16_t)configHeader[3] << 8);
    if (totalLength < 9 || totalLength > MSD_MAX_CONFIG_BYTES) {
      Serial.print("[MSD] Unsupported configuration descriptor length: ");
      Serial.println(totalLength);
      continue;
    }

    uint8_t configData[MSD_MAX_CONFIG_BYTES] = {0};
    uint16_t configReceived = 0;
    if (!enumControlGetDescriptor(ENUM_NEW_DEVICE_ADDRESS,
                                  enumStoredEp0PacketSize,
                                  0x02, (uint8_t)configIndex, 0, totalLength,
                                  configData, sizeof(configData),
                                  configReceived, "MSD full configuration descriptor")) {
      Serial.print("[MSD] Failed to read configuration index ");
      Serial.println(configIndex);
      continue;
    }

    if (configReceived < totalLength || configData[1] != 0x02 || configData[0] < 9) {
      Serial.print("[MSD] Incomplete/malformed configuration index ");
      Serial.println(configIndex);
      continue;
    }

    uint8_t configurationValue = configData[5];
    if (configurationValue == 0) {
      Serial.println("[MSD] Invalid bConfigurationValue=0; skipping configuration.");
      continue;
    }
    bool currentInterfaceIsBot = false;
    uint8_t currentInterfaceNumber = 0;
    uint8_t currentAlternateSetting = 0;
    uint8_t bulkOutAddress = 0;
    uint8_t bulkInAddress = 0;
    uint8_t bulkOutMaxPacket = 0;
    uint8_t bulkInMaxPacket = 0;

    uint16_t offset = 0;
    while (offset + 2 <= configReceived) {
      uint8_t descriptorLength = configData[offset];
      uint8_t descriptorType = configData[offset + 1];
      if (descriptorLength < 2 || offset + descriptorLength > configReceived) {
        Serial.println("[MSD] Malformed descriptor; abandoning this configuration.");
        break;
      }

      if (descriptorType == 0x04) {
        currentInterfaceIsBot = false;
        bulkOutAddress = 0;
        bulkInAddress = 0;
        bulkOutMaxPacket = 0;
        bulkInMaxPacket = 0;

        if (descriptorLength >= 9) {
          currentInterfaceNumber = configData[offset + 2];
          currentAlternateSetting = configData[offset + 3];
          uint8_t interfaceClass = configData[offset + 5];
          uint8_t interfaceSubClass = configData[offset + 6];
          uint8_t interfaceProtocol = configData[offset + 7];

          // BOT interfaces are selected at alternate setting zero. Other
          // alternate settings are not implicitly activated by SET_CONFIGURATION.
          currentInterfaceIsBot =
            (currentAlternateSetting == 0 &&
             interfaceClass == 0x08 &&
             interfaceSubClass == 0x06 &&
             interfaceProtocol == 0x50);
        }
      }
      else if (descriptorType == 0x05 && descriptorLength >= 7 &&
               currentInterfaceIsBot) {
        uint8_t endpointAddress = configData[offset + 2];
        uint8_t attributes = configData[offset + 3];
        uint16_t rawMaxPacket = (uint16_t)configData[offset + 4] |
                                ((uint16_t)configData[offset + 5] << 8);
        uint16_t maxPacket = rawMaxPacket & 0x07FF;
        bool isBulk = ((attributes & 0x03) == 0x02);
        bool validAddress = ((endpointAddress & 0x70) == 0 &&
                             (endpointAddress & 0x0F) != 0 &&
                             rawMaxPacket == maxPacket);

        if (isBulk && validAddress && msdValidFullSpeedBulkPacketSize(maxPacket)) {
          if (endpointAddress & 0x80) {
            if (bulkInAddress == 0) {
              bulkInAddress = endpointAddress;
              bulkInMaxPacket = (uint8_t)maxPacket;
            }
          } else {
            if (bulkOutAddress == 0) {
              bulkOutAddress = endpointAddress;
              bulkOutMaxPacket = (uint8_t)maxPacket;
            }
          }

          if (bulkInAddress != 0 && bulkOutAddress != 0) {
            selection.found = true;
            selection.configurationIndex = (uint8_t)configIndex;
            selection.configurationValue = configurationValue;
            selection.interfaceNumber = currentInterfaceNumber;
            selection.alternateSetting = currentAlternateSetting;
            selection.bulkOutAddress = bulkOutAddress;
            selection.bulkInAddress = bulkInAddress;
            selection.bulkOutMaxPacket = bulkOutMaxPacket;
            selection.bulkInMaxPacket = bulkInMaxPacket;

            Serial.println();
            Serial.println("[MSD] Compatible Mass Storage BOT interface found:");
            Serial.print("  Configuration index/value: ");
            Serial.print(selection.configurationIndex);
            Serial.print(" / ");
            Serial.println(selection.configurationValue);
            Serial.print("  Interface / alternate: ");
            Serial.print(selection.interfaceNumber);
            Serial.print(" / ");
            Serial.println(selection.alternateSetting);
            Serial.print("  Bulk OUT: 0x");
            printHexByte(selection.bulkOutAddress);
            Serial.print("  MPS=");
            Serial.println(selection.bulkOutMaxPacket);
            Serial.print("  Bulk IN:  0x");
            printHexByte(selection.bulkInAddress);
            Serial.print("  MPS=");
            Serial.println(selection.bulkInMaxPacket);
            return true;
          }
        }
      }

      offset += descriptorLength;
    }
  }

  Serial.println("[MSD] No usable SCSI transparent / Bulk-Only interface found.");
  Serial.println("      Required: interface 08/06/50 and one valid Bulk IN + Bulk OUT endpoint.");
  return false;
}

// Bulk endpoints have independent DATA toggles. NAK does not advance a toggle.
// BOT wrapper/block data is packetized according to the discovered endpoint MPS.
EnumTransferResult msdBulkTransferWithRetry(uint8_t pid,
                                            uint8_t endpoint,
                                            bool directionOut,
                                            bool data1,
                                            uint8_t length,
                                            const uint8_t *outData,
                                            uint32_t timeoutMs) {
  EnumTransferResult result;
  memset(&result, 0, sizeof(result));
  result.txLeft = 0xFF;
  result.attemptLimit = 0; // Bulk retry is governed by elapsed time, not an 8-bit cap.

  uint32_t startMs = millis();
  uint32_t attempts = 0;
  uint32_t nakCount = 0;

  do {
    result = enumIssueChannelAEndpoint(
      pid, endpoint, directionOut, data1,
      ENUM_NEW_DEVICE_ADDRESS, ENUM_BUFFER_A,
      length, outData);
    attempts++;
    result.attemptLimit = 0;

    if (!result.started || !result.done)
      break;

    if (result.ack && result.sequenceOK)
      break;

    if (result.hostStatus & ENUM_HOST_NAK) {
      nakCount++;
      delay(1);
      continue;
    }

    // STALL, DATA toggle mismatch, or another controller error is terminal.
    break;
  } while ((uint32_t)(millis() - startMs) < timeoutMs);

  result.attempts = attempts;
  result.nakCount = nakCount;
  result.attemptLimit = 0; // Printed as time-limited rather than N/N attempts.
  return result;
}

bool msdBulkOutBuffer(uint8_t endpointAddress,
                      uint8_t maxPacketSize,
                      bool &data1,
                      const uint8_t *data,
                      uint16_t length,
                      const char *description) {
  uint16_t offset = 0;
  uint8_t packetNumber = 0;
  uint8_t endpointNumber = endpointAddress & 0x0F;

  while (offset < length) {
    uint16_t remaining = length - offset;
    uint8_t packetLength = (remaining < maxPacketSize)
                         ? (uint8_t)remaining : maxPacketSize;
    char label[72];
    snprintf(label, sizeof(label), "%s OUT 0x%02X packet %u",
             description, endpointAddress, (unsigned int)(packetNumber + 1));

    EnumTransferResult result = msdBulkTransferWithRetry(
      ENUM_PID_OUT, endpointNumber, true, data1,
      packetLength, data + offset, MSD_BULK_TIMEOUT_MS);
    enumPrintTransferResult(label, result, ENUM_PID_OUT, true,
                            data1, packetLength);
    if (!enumTransferSucceeded(result) || result.txLeft != 0) {
      Serial.println("[MSD] Bulk OUT packet failed; BOT command aborted.");
      return false;
    }

    // Each successfully ACKed USB packet advances the pipe's DATA toggle.
    data1 = !data1;
    offset += packetLength;
    packetNumber++;
  }
  return true;
}

bool msdBulkInBuffer(uint8_t endpointAddress,
                     uint8_t maxPacketSize,
                     bool &data1,
                     uint8_t *destination,
                     uint16_t expectedLength,
                     uint16_t &actualLength,
                     const char *description,
                     uint32_t timeoutMs = MSD_BULK_TIMEOUT_MS) {
  actualLength = 0;
  uint8_t endpointNumber = endpointAddress & 0x0F;
  uint8_t packetNumber = 0;

  while (actualLength < expectedLength) {
    char label[72];
    snprintf(label, sizeof(label), "%s IN 0x%02X packet %u",
             description, endpointAddress, (unsigned int)(packetNumber + 1));

    // Request one full endpoint packet. The device terminates the data phase
    // with a short packet when the payload is not a multiple of wMaxPacketSize.
    EnumTransferResult result = msdBulkTransferWithRetry(
      ENUM_PID_IN, endpointNumber, false, data1,
      maxPacketSize, NULL, timeoutMs);
    enumPrintTransferResult(label, result, ENUM_PID_IN, false,
                            data1, maxPacketSize);

    if (!enumTransferSucceeded(result) || result.txLeft > maxPacketSize) {
      Serial.println("[MSD] Bulk IN packet failed; BOT command aborted.");
      return false;
    }

    uint8_t packetReceived = (uint8_t)(maxPacketSize - result.txLeft);
    uint16_t remaining = expectedLength - actualLength;
    if (packetReceived > remaining) {
      Serial.println("[MSD] Device returned more data than the BOT transfer length.");
      return false;
    }

    for (uint8_t i = 0; i < packetReceived; i++)
      destination[actualLength + i] = slReadRAM(ENUM_BUFFER_A + i);

    actualLength += packetReceived;
    data1 = !data1;
    packetNumber++;

    Serial.print("  Packet bytes=");
    Serial.print(packetReceived);
    Serial.print(" total=");
    Serial.print(actualLength);
    Serial.print("/");
    Serial.println(expectedLength);

    // A short packet marks the end of this BOT data phase. A zero-length
    // packet also terminates it and is not treated as a controller error.
    if (packetReceived < maxPacketSize)
      break;
  }
  return true;
}

bool msdGetMaxLun(uint8_t interfaceNumber, uint8_t &maxLun) {
  maxLun = 0;

  const uint8_t setupPacket[8] = {
    0xA1, 0xFE,             // Class / Interface / Device-to-host / GET_MAX_LUN
    0x00, 0x00,             // wValue
    interfaceNumber, 0x00,  // wIndex = dynamically discovered interface
    0x01, 0x00              // wLength = 1
  };

  Serial.println();
  Serial.print("[BOT] GET_MAX_LUN, interface ");
  Serial.println(interfaceNumber);

  EnumTransferResult setupResult = enumIssueChannelA_with_retry(
    ENUM_PID_SETUP, true, false, ENUM_NEW_DEVICE_ADDRESS,
    ENUM_BUFFER_A, 8, setupPacket);
  enumPrintTransferResult("GET_MAX_LUN SETUP, DATA0",
                          setupResult, ENUM_PID_SETUP, true, false, 8);
  if (!enumTransferSucceeded(setupResult)) {
    Serial.println("GET_MAX_LUN: SETUP stage failed.");
    return false;
  }

  EnumTransferResult dataResult = enumIssueChannelA_with_retry(
    ENUM_PID_IN, false, true, ENUM_NEW_DEVICE_ADDRESS,
    ENUM_BUFFER_A, 1, NULL);
  enumPrintTransferResult("GET_MAX_LUN DATA IN, DATA1, 1 byte",
                          dataResult, ENUM_PID_IN, false, true, 1);

  if (dataResult.started && dataResult.done &&
      (dataResult.hostStatus & ENUM_HOST_STALL)) {
    // BOT explicitly permits STALL for an unsupported GET_MAX_LUN request.
    Serial.println("GET_MAX_LUN returned STALL; BOT default is LUN 0.");
    maxLun = 0;
    return true;
  }

  if (!enumTransferSucceeded(dataResult) || dataResult.txLeft > 1) {
    Serial.println("GET_MAX_LUN: DATA stage failed (not an accepted STALL).");
    return false;
  }

  uint8_t received = (uint8_t)(1 - dataResult.txLeft);
  if (received != 1) {
    Serial.print("GET_MAX_LUN: expected 1 byte, received ");
    Serial.println(received);
    return false;
  }

  maxLun = slReadRAM(ENUM_BUFFER_A);
  EnumTransferResult statusResult = enumIssueChannelA_with_retry(
    ENUM_PID_OUT, true, true, ENUM_NEW_DEVICE_ADDRESS,
    ENUM_BUFFER_A, 0, NULL);
  enumPrintTransferResult("GET_MAX_LUN STATUS OUT, DATA1, zero-length",
                          statusResult, ENUM_PID_OUT, true, true, 0);
  if (!enumTransferSucceeded(statusResult)) {
    Serial.println("GET_MAX_LUN: STATUS OUT failed.");
    return false;
  }

  Serial.print("GET_MAX_LUN value: ");
  Serial.println(maxLun);
  if (maxLun > 15) {
    Serial.println("GET_MAX_LUN returned an invalid value (>15).");
    return false;
  }
  return true;
}


// ==========================================
// GENERIC SCSI COMMAND EXECUTION + FIRST-SECTOR READ
// ==========================================
#define MSD_MAX_LOGICAL_BLOCK_SIZE 4096
#define MSD_READ_TAG_BASE          0x32524C53UL  // little-endian bytes spell "SLR2"

void msdStoreLe32(uint8_t *p, uint32_t value) {
  p[0] = (uint8_t)(value & 0xFF);
  p[1] = (uint8_t)((value >> 8) & 0xFF);
  p[2] = (uint8_t)((value >> 16) & 0xFF);
  p[3] = (uint8_t)((value >> 24) & 0xFF);
}

uint32_t msdReadBe32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) |
         ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) |
         (uint32_t)p[3];
}

void msdPrintUint64(uint64_t value) {
  char digits[21];
  uint8_t count = 0;
  do {
    digits[count++] = (char)('0' + (value % 10));
    value /= 10;
  } while (value != 0 && count < sizeof(digits));
  while (count > 0)
    Serial.print(digits[--count]);
}

// Executes one SCSI command through BOT. The caller owns the independent
// Bulk IN/OUT DATA toggles and must preserve them between successive commands.
// This tester supports BOT data-IN, data-OUT, and no-data SCSI commands.
bool msdBotExecuteScsi(const MsdBotInterface &bot,
                       uint8_t lun,
                       const char *commandName,
                       const uint8_t *cdb,
                       uint8_t cdbLength,
                       uint32_t transferLength,
                       bool dataIn,
                       uint8_t *dataBuffer,
                       uint16_t dataCapacity,
                       bool &bulkOutData1,
                       bool &bulkInData1,
                       uint32_t tag,
                       uint16_t &actualLength,
                       uint8_t &cswStatus) {
  actualLength = 0;
  cswStatus = 0xFF;

  if (cdb == NULL || cdbLength == 0 || cdbLength > 16 || lun > 15) {
    Serial.println("[BOT] Invalid SCSI command parameters.");
    return false;
  }
  if (transferLength > 0 && (dataBuffer == NULL ||
                             transferLength > dataCapacity ||
                             transferLength > 0xFFFFUL)) {
    Serial.println("[BOT] Invalid/oversized data phase or insufficient data buffer.");
    return false;
  }

  uint8_t cbw[MSD_CBW_LENGTH] = {0};
  msdStoreLe32(&cbw[0], MSD_CBW_SIGNATURE);
  msdStoreLe32(&cbw[4], tag);
  msdStoreLe32(&cbw[8], transferLength);
  cbw[12] = (transferLength == 0) ? 0x00 : (dataIn ? 0x80 : 0x00);
  cbw[13] = lun;
  cbw[14] = cdbLength;
  memcpy(&cbw[15], cdb, cdbLength);

  Serial.println();
  Serial.print("========== SCSI ");
  Serial.print(commandName);
  Serial.println(" ==========");
  Serial.print("CBW tag: 0x");
  for (int8_t shift = 24; shift >= 0; shift -= 8)
    printHexByte((uint8_t)(tag >> shift));
  Serial.print("  LUN=");
  Serial.print(lun);
  Serial.print("  CDB length=");
  Serial.print(cdbLength);
  Serial.print("  data length=");
  Serial.println((unsigned long)transferLength);

  Serial.println("[BOT 1/3] Send CBW");
  if (!msdBulkOutBuffer(bot.bulkOutAddress, bot.bulkOutMaxPacket,
                        bulkOutData1, cbw, sizeof(cbw), "SCSI CBW")) {
    Serial.println("[BOT] CBW failed; command aborted.");
    return false;
  }

  if (transferLength > 0) {
    if (dataIn) {
      Serial.println("[BOT 2/3] Receive data-IN phase");
      if (!msdBulkInBuffer(bot.bulkInAddress, bot.bulkInMaxPacket,
                           bulkInData1, dataBuffer, (uint16_t)transferLength,
                           actualLength, commandName)) {
        Serial.println("[BOT] Data-IN phase failed; command aborted before CSW.");
        return false;
      }
    } else {
      Serial.println("[BOT 2/3] Send data-OUT phase");
      if (!msdBulkOutBuffer(bot.bulkOutAddress, bot.bulkOutMaxPacket,
                            bulkOutData1, dataBuffer, (uint16_t)transferLength,
                            commandName)) {
        Serial.println("[BOT] Data-OUT phase failed; command aborted before CSW.");
        return false;
      }
      // Every byte of the declared data phase was ACKed by the target.
      actualLength = (uint16_t)transferLength;
    }
  } else {
    Serial.println("[BOT 2/3] No data phase for this SCSI command.");
  }

  Serial.println("[BOT 3/3] Receive Command Status Wrapper");
  uint8_t csw[MSD_CSW_LENGTH] = {0};
  uint16_t cswLength = 0;
  if (!msdBulkInBuffer(bot.bulkInAddress, bot.bulkInMaxPacket,
                       bulkInData1, csw, MSD_CSW_LENGTH,
                       cswLength, "BOT CSW", MSD_CSW_TIMEOUT_MS)) {
    Serial.println("[BOT] CSW transfer failed.");
    return false;
  }
  if (cswLength != MSD_CSW_LENGTH) {
    Serial.print("[BOT] Invalid CSW length: ");
    Serial.println(cswLength);
    return false;
  }

  uint32_t signature = (uint32_t)csw[0] |
                       ((uint32_t)csw[1] << 8) |
                       ((uint32_t)csw[2] << 16) |
                       ((uint32_t)csw[3] << 24);
  uint32_t returnedTag = (uint32_t)csw[4] |
                         ((uint32_t)csw[5] << 8) |
                         ((uint32_t)csw[6] << 16) |
                         ((uint32_t)csw[7] << 24);
  uint32_t residue = (uint32_t)csw[8] |
                     ((uint32_t)csw[9] << 8) |
                     ((uint32_t)csw[10] << 16) |
                     ((uint32_t)csw[11] << 24);

  Serial.print("CSW signature: 0x");
  for (int8_t shift = 24; shift >= 0; shift -= 8)
    printHexByte((uint8_t)(signature >> shift));
  Serial.print("  tag: 0x");
  for (int8_t shift = 24; shift >= 0; shift -= 8)
    printHexByte((uint8_t)(returnedTag >> shift));
  Serial.print("  residue: ");
  Serial.println((unsigned long)residue);
  Serial.print("CSW status: 0x");
  printHexByte(csw[12]);
  Serial.println();

  if (signature != MSD_CSW_SIGNATURE) {
    Serial.println("[BOT] ERROR: CSW signature mismatch.");
    return false;
  }
  if (returnedTag != tag) {
    Serial.println("[BOT] ERROR: CSW tag does not match this command.");
    return false;
  }
  if (residue > transferLength) {
    Serial.println("[BOT] ERROR: CSW residue exceeds requested transfer length.");
    return false;
  }
  if (residue != (transferLength - actualLength)) {
    Serial.println("[BOT] WARNING: CSW residue differs from host-observed data shortfall.");
  }
  if (csw[12] > 2) {
    Serial.println("[BOT] ERROR: Invalid CSW status value.");
    return false;
  }

  cswStatus = csw[12];
  if (cswStatus == 0x00) {
    Serial.print("SCSI ");
    Serial.print(commandName);
    Serial.println(": COMMAND PASSED.");
  } else if (cswStatus == 0x01) {
    Serial.print("SCSI ");
    Serial.print(commandName);
    Serial.println(": CHECK CONDITION / COMMAND FAILED.");
  } else {
    Serial.print("SCSI ");
    Serial.print(commandName);
    Serial.println(": PHASE ERROR; BOT reset recovery may be required.");
  }
  return true;
}

const char *msdSenseKeyName(uint8_t key) {
  switch (key & 0x0F) {
    case 0x00: return "No Sense";
    case 0x01: return "Recovered Error";
    case 0x02: return "Not Ready";
    case 0x03: return "Medium Error";
    case 0x04: return "Hardware Error";
    case 0x05: return "Illegal Request";
    case 0x06: return "Unit Attention";
    case 0x07: return "Data Protect";
    case 0x0B: return "Aborted Command";
    default:   return "Other / vendor-specific";
  }
}

bool msdRequestSenseAndPrint(const MsdBotInterface &bot,
                             uint8_t lun,
                             bool &bulkOutData1,
                             bool &bulkInData1,
                             uint32_t &nextTag,
                             uint8_t *senseKeyOut = NULL,
                             uint8_t *ascOut = NULL,
                             uint8_t *ascqOut = NULL) {
  if (senseKeyOut) *senseKeyOut = 0xFF;
  if (ascOut) *ascOut = 0xFF;
  if (ascqOut) *ascqOut = 0xFF;
  uint8_t cdb[6] = {0x03, 0x00, 0x00, 0x00, 18, 0x00};
  uint8_t sense[18] = {0};
  uint16_t actual = 0;
  uint8_t status = 0xFF;

  Serial.println();
  Serial.println("[SCSI] REQUEST SENSE after CHECK CONDITION");
  if (!msdBotExecuteScsi(bot, lun, "REQUEST SENSE", cdb, sizeof(cdb),
                         sizeof(sense), true, sense, sizeof(sense),
                         bulkOutData1, bulkInData1, nextTag++, actual, status)) {
    Serial.println("REQUEST SENSE transport failed.");
    return false;
  }
  if (status != 0x00) {
    Serial.println("REQUEST SENSE itself did not pass.");
    return false;
  }
  if (actual < 4) {
    Serial.println("REQUEST SENSE returned too few bytes to decode.");
    return false;
  }

  uint8_t responseCode = sense[0] & 0x7F;
  uint8_t senseKey = 0;
  uint8_t asc = 0;
  uint8_t ascq = 0;
  bool decoded = false;

  if ((responseCode == 0x70 || responseCode == 0x71) && actual >= 14) {
    senseKey = sense[2] & 0x0F;
    asc = sense[12];
    ascq = sense[13];
    decoded = true;
  } else if ((responseCode == 0x72 || responseCode == 0x73) && actual >= 4) {
    senseKey = sense[1] & 0x0F;
    asc = sense[2];
    ascq = sense[3];
    decoded = true;
  }

  if (decoded) {
    if (senseKeyOut) *senseKeyOut = senseKey;
    if (ascOut) *ascOut = asc;
    if (ascqOut) *ascqOut = ascq;
  }

  Serial.print("Sense response code: 0x");
  printHexByte(responseCode);
  Serial.println();
  if (decoded) {
    Serial.print("Sense key: 0x");
    printHexByte(senseKey);
    Serial.print(" ( ");
    Serial.print(msdSenseKeyName(senseKey));
    Serial.println(" )");
    Serial.print("ASC / ASCQ: 0x");
    printHexByte(asc);
    Serial.print(" / 0x");
    printHexByte(ascq);
    Serial.println();
  } else {
    Serial.println("Unknown or truncated REQUEST SENSE format; raw bytes follow.");
  }

  Serial.println("REQUEST SENSE data:");
  for (uint16_t i = 0; i < actual; i++) {
    printHexByte(sense[i]);
    if ((i & 0x0F) == 0x0F || i + 1 == actual) Serial.println();
    else Serial.print(' ');
  }
  return true;
}

void msdPrintSectorHexDump(const uint8_t *data, uint16_t length, uint32_t lba = 0) {
  Serial.println();
  Serial.print("LBA ");
  Serial.print((unsigned long)lba);
  Serial.println(" block dump (hex + ASCII):");
  for (uint16_t offset = 0; offset < length; offset += 16) {
    printHexByte((uint8_t)(offset >> 8));
    printHexByte((uint8_t)offset);
    Serial.print("  ");

    uint8_t lineLength = (uint8_t)(((length - offset) < 16) ?
                                   (length - offset) : 16);
    for (uint8_t i = 0; i < 16; i++) {
      if (i < lineLength) {
        printHexByte(data[offset + i]);
        Serial.print(' ');
      } else {
        Serial.print("   ");
      }
    }
    Serial.print(" | ");
    for (uint8_t i = 0; i < lineLength; i++) {
      uint8_t c = data[offset + i];
      Serial.print((c >= 0x20 && c <= 0x7E) ? (char)c : '.');
    }
    Serial.println();
  }

  bool foundRdsK = false;
  for (uint16_t i = 0; i + 4 <= length; i++) {
    if (data[i] == 'R' && data[i + 1] == 'D' &&
        data[i + 2] == 'S' && data[i + 3] == 'K') {
      Serial.print("Amiga RDB signature 'RDSK' found at byte offset 0x");
      printHexByte((uint8_t)(i >> 8));
      printHexByte((uint8_t)i);
      Serial.println();
      foundRdsK = true;
    }
  }
  if (!foundRdsK)
    Serial.println("No 'RDSK' signature in this sector (RDB may be in a later early block). ");
  if (length >= 4 && data[0] == 'D' && data[1] == 'O' && data[2] == 'S')
    Serial.println("Amiga DOS bootblock signature 'DOS' found at byte offset 0.");
}

void poseidonMassStorageReadFirstSector() {
  Serial.println();
  Serial.println("========================================");
  Serial.println(" USB MASS STORAGE - SCSI MEDIA READ TEST");
  Serial.println(" TEST UNIT READY / REQUEST SENSE / READ CAPACITY / READ(10)");
  Serial.println("========================================");
  Serial.println("READ-ONLY operation. No write commands are issued.");
  Serial.println("BOT CSW wait: up to 10 seconds per packet; NAK counter is 32-bit/time-limited.");

  if (!enumAddress1Ready) {
    Serial.println("ERROR: Run POSEIDON, then ENUM18 successfully first.");
    return;
  }

  MsdBotInterface bot;
  if (!msdFindBotInterface(bot)) {
    Serial.println("STOP: no compatible Mass Storage BOT interface was found.");
    return;
  }

  // Selecting the discovered configuration initializes both bulk pipes to DATA0.
  char configLabel[64];
  snprintf(configLabel, sizeof(configLabel), "SET_CONFIGURATION(%u) - media read test",
           bot.configurationValue);
  if (!enumControlNoData(0x00, 0x09, bot.configurationValue, 0, configLabel)) {
    Serial.println("STOP: SET_CONFIGURATION failed.");
    return;
  }

  uint8_t maxLun = 0;
  if (!msdGetMaxLun(bot.interfaceNumber, maxLun)) {
    Serial.println("STOP: GET_MAX_LUN failed.");
    return;
  }
  Serial.println("This test selects LUN 0. Other LUNs are not probed in this pass.");
  const uint8_t lun = 0;

  // These are independent endpoint data toggles. Preserve them across every
  // BOT command in this session; only SET_CONFIGURATION resets them to DATA0.
  bool bulkOutData1 = false;
  bool bulkInData1 = false;
  uint32_t nextTag = MSD_READ_TAG_BASE;
  uint16_t actual = 0;
  uint8_t status = 0xFF;

  // 1) TEST UNIT READY (six-byte CDB, no data phase).
  // A freshly powered reader commonly reports one Unit Attention
  // (ASC/ASCQ 28/00: medium may have changed). REQUEST SENSE acknowledges
  // that attention; retry TUR a limited number of times afterwards.
  const uint8_t testUnitReadyCdb[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  Serial.println();
  Serial.println("[1/4] TEST UNIT READY - checking whether the selected medium is ready");

  bool mediumReady = false;
  const uint8_t maxTurUnitAttentionRetries = 2;
  uint8_t unitAttentionRetries = 0;

  while (!mediumReady) {
    if (!msdBotExecuteScsi(bot, lun, "TEST UNIT READY",
                           testUnitReadyCdb, sizeof(testUnitReadyCdb),
                           0, false, NULL, 0,
                           bulkOutData1, bulkInData1, nextTag++, actual, status)) {
      Serial.println("STOP: TEST UNIT READY transport failed.");
      return;
    }

    if (status == 0x00) {
      mediumReady = true;
      Serial.println("[SCSI] Medium is ready.");
      break;
    }

    if (status != 0x01) {
      Serial.print("STOP: TEST UNIT READY returned unexpected SCSI status 0x");
      printHexByte(status);
      Serial.println(".");
      return;
    }

    uint8_t senseKey = 0xFF;
    uint8_t asc = 0xFF;
    uint8_t ascq = 0xFF;
    if (!msdRequestSenseAndPrint(bot, lun, bulkOutData1, bulkInData1,
                                 nextTag, &senseKey, &asc, &ascq)) {
      Serial.println("STOP: REQUEST SENSE failed; medium readiness is unknown.");
      return;
    }

    if (senseKey == 0x06 && asc == 0x28 && ascq == 0x00) {
      if (unitAttentionRetries >= maxTurUnitAttentionRetries) {
        Serial.println("STOP: medium-change UNIT ATTENTION persisted after retries.");
        Serial.println("No capacity query or sector read attempted.");
        return;
      }

      unitAttentionRetries++;
      Serial.print("[SCSI] UNIT ATTENTION 06/28/00 acknowledged; retrying TEST UNIT READY (");
      Serial.print(unitAttentionRetries);
      Serial.print("/");
      Serial.print(maxTurUnitAttentionRetries);
      Serial.println(").");
      delay(20);
      continue;
    }

    Serial.println("STOP: CHECK CONDITION is not the handled medium-change UNIT ATTENTION 06/28/00.");
    Serial.println("No capacity query or sector read attempted.");
    return;
  }

  // 2) READ CAPACITY (10), LBA 0, PMI 0.
  const uint8_t readCapacityCdb[10] = {
    0x25, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
  };
  uint8_t capacityData[8] = {0};
  Serial.println();
  Serial.println("[2/4] READ CAPACITY (10) - querying last LBA and logical block size");
  if (!msdBotExecuteScsi(bot, lun, "READ CAPACITY (10)",
                         readCapacityCdb, sizeof(readCapacityCdb),
                         sizeof(capacityData), true,
                         capacityData, sizeof(capacityData),
                         bulkOutData1, bulkInData1, nextTag++, actual, status)) {
    Serial.println("STOP: READ CAPACITY transport failed.");
    return;
  }
  if (status != 0x00) {
    if (status == 0x01)
      msdRequestSenseAndPrint(bot, lun, bulkOutData1, bulkInData1, nextTag);
    Serial.println("STOP: READ CAPACITY did not pass.");
    return;
  }
  if (actual != sizeof(capacityData)) {
    Serial.print("STOP: READ CAPACITY returned ");
    Serial.print(actual);
    Serial.println(" bytes instead of 8.");
    return;
  }

  uint32_t lastLba = msdReadBe32(&capacityData[0]);
  uint32_t blockLength = msdReadBe32(&capacityData[4]);
  Serial.print("Last logical block address: 0x");
  for (int8_t shift = 24; shift >= 0; shift -= 8)
    printHexByte((uint8_t)(lastLba >> shift));
  Serial.println();
  Serial.print("Logical block size: ");
  Serial.print((unsigned long)blockLength);
  Serial.println(" bytes");
  if (lastLba == 0xFFFFFFFFUL) {
    Serial.println("READ CAPACITY(10) reports the 0xFFFFFFFF sentinel; full capacity needs READ CAPACITY(16).");
  } else {
    Serial.print("Logical block count: ");
    Serial.println((unsigned long)(lastLba + 1UL));
    uint64_t capacityBytes = ((uint64_t)lastLba + 1ULL) * (uint64_t)blockLength;
    Serial.print("Reported medium capacity: ");
    msdPrintUint64(capacityBytes);
    Serial.println(" bytes");
  }

  if (blockLength == 0 || blockLength > MSD_MAX_LOGICAL_BLOCK_SIZE) {
    Serial.print("STOP: logical block size must be 1.. ");
    Serial.print(MSD_MAX_LOGICAL_BLOCK_SIZE);
    Serial.println(" bytes for this test buffer.");
    return;
  }

  // 3) READ(10), exactly one logical block beginning at LBA 0.
  const uint8_t read10Cdb[10] = {
    0x28, 0x00,             // READ(10), no DPO/FUA
    0x00, 0x00, 0x00, 0x00, // LBA = 0
    0x00,                   // group number
    0x00, 0x01,             // transfer length = 1 logical block
    0x00                    // control
  };
  uint8_t sector[MSD_MAX_LOGICAL_BLOCK_SIZE] = {0};
  Serial.println();
  Serial.println("[3/4] READ(10) - reading exactly one logical block at LBA 0");
  if (!msdBotExecuteScsi(bot, lun, "READ(10) LBA 0",
                         read10Cdb, sizeof(read10Cdb),
                         blockLength, true, sector, sizeof(sector),
                         bulkOutData1, bulkInData1, nextTag++, actual, status)) {
    Serial.println("STOP: READ(10) transport failed.");
    return;
  }
  if (status != 0x00) {
    if (status == 0x01)
      msdRequestSenseAndPrint(bot, lun, bulkOutData1, bulkInData1, nextTag);
    Serial.println("STOP: READ(10) did not pass.");
    return;
  }
  if (actual != blockLength) {
    Serial.print("STOP: short sector read; expected ");
    Serial.print((unsigned long)blockLength);
    Serial.print(" bytes, received ");
    Serial.println(actual);
    return;
  }

  Serial.println();
  Serial.println("[4/4] SECTOR DATA RECEIVED AND CSW VALIDATED");
  Serial.print("LBA 0 successfully read: ");
  Serial.print(actual);
  Serial.println(" bytes");
  msdPrintSectorHexDump(sector, actual);

  Serial.println();
  Serial.println("========================================");
  Serial.println(" SCSI MEDIA READ TEST PASSED");
  Serial.println(" TEST UNIT READY: passed");
  Serial.println(" READ CAPACITY (10): passed");
  Serial.println(" READ (10) LBA 0: passed");
  Serial.println(" No media writes were issued.");
  Serial.println("========================================");
}


// ==========================================
// SAFE ONE-BLOCK SCSI WRITE + READ-BACK TEST
// ==========================================
#define MSD_WRITE_TAG_BASE 0x32525753UL  // little-endian bytes spell "SWR2"

void poseidonMassStorageWriteVerifyTest() {
  Serial.println();
  Serial.println("========================================");
  Serial.println(" USB MASS STORAGE - WRITE(10) + READ-BACK TEST");
  Serial.println(" One logical block; no filesystem required");
  Serial.println("========================================");
  Serial.println("WARNING: This writes one test-pattern block to the selected medium.");
  Serial.println("Use a blank/sacrificial USB device with no valuable data.");
  Serial.println("The test block is chosen near the middle of the device and is NOT restored.");

  if (!enumAddress1Ready) {
    Serial.println("ERROR: Run POSEIDON, then ENUM18 successfully first.");
    return;
  }

  MsdBotInterface bot;
  if (!msdFindBotInterface(bot)) {
    Serial.println("STOP: no compatible Mass Storage BOT interface was found.");
    return;
  }

  char configLabel[64];
  snprintf(configLabel, sizeof(configLabel), "SET_CONFIGURATION(%u) - write test",
           bot.configurationValue);
  if (!enumControlNoData(0x00, 0x09, bot.configurationValue, 0, configLabel)) {
    Serial.println("STOP: SET_CONFIGURATION failed.");
    return;
  }

  uint8_t maxLun = 0;
  if (!msdGetMaxLun(bot.interfaceNumber, maxLun)) {
    Serial.println("STOP: GET_MAX_LUN failed.");
    return;
  }
  const uint8_t lun = 0;
  Serial.println("[MSD] Using LUN 0.");

  // Bulk IN and OUT have independent DATA toggle state, preserved for the
  // full BOT session. SET_CONFIGURATION starts both pipes at DATA0.
  bool bulkOutData1 = false;
  bool bulkInData1 = false;
  uint32_t nextTag = MSD_WRITE_TAG_BASE;
  uint16_t actual = 0;
  uint8_t status = 0xFF;

  // Check media readiness and acknowledge the common medium-change Unit Attention.
  const uint8_t turCdb[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  bool mediumReady = false;
  uint8_t unitAttentionRetries = 0;
  const uint8_t maxUnitAttentionRetries = 2;
  while (!mediumReady) {
    if (!msdBotExecuteScsi(bot, lun, "TEST UNIT READY", turCdb, sizeof(turCdb),
                           0, false, NULL, 0,
                           bulkOutData1, bulkInData1, nextTag++, actual, status)) {
      Serial.println("STOP: TEST UNIT READY transport failed.");
      return;
    }
    if (status == 0x00) {
      mediumReady = true;
      break;
    }
    if (status != 0x01) {
      Serial.print("STOP: unexpected TEST UNIT READY status 0x");
      printHexByte(status);
      Serial.println();
      return;
    }

    uint8_t senseKey = 0xFF, asc = 0xFF, ascq = 0xFF;
    if (!msdRequestSenseAndPrint(bot, lun, bulkOutData1, bulkInData1,
                                 nextTag, &senseKey, &asc, &ascq)) {
      Serial.println("STOP: REQUEST SENSE failed.");
      return;
    }
    if (senseKey == 0x06 && asc == 0x28 && ascq == 0x00 &&
        unitAttentionRetries < maxUnitAttentionRetries) {
      unitAttentionRetries++;
      Serial.println("[SCSI] Medium-change UNIT ATTENTION acknowledged; retrying TUR.");
      delay(20);
      continue;
    }
    Serial.println("STOP: medium is not ready; no write attempted.");
    return;
  }
  Serial.println("[SCSI] Medium is ready.");

  // Read capacity; only one logical block is written by this test.
  const uint8_t capacityCdb[10] = {
    0x25, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
  };
  uint8_t capacityData[8] = {0};
  if (!msdBotExecuteScsi(bot, lun, "READ CAPACITY (10)", capacityCdb,
                         sizeof(capacityCdb), sizeof(capacityData), true,
                         capacityData, sizeof(capacityData),
                         bulkOutData1, bulkInData1, nextTag++, actual, status)) {
    Serial.println("STOP: READ CAPACITY transport failed; no write attempted.");
    return;
  }
  if (status != 0x00 || actual != sizeof(capacityData)) {
    if (status == 0x01)
      msdRequestSenseAndPrint(bot, lun, bulkOutData1, bulkInData1, nextTag);
    Serial.println("STOP: READ CAPACITY did not complete; no write attempted.");
    return;
  }

  uint32_t lastLba = msdReadBe32(&capacityData[0]);
  uint32_t blockLength = msdReadBe32(&capacityData[4]);
  Serial.print("Last LBA: ");
  Serial.println((unsigned long)lastLba);
  Serial.print("Logical block size: ");
  Serial.print((unsigned long)blockLength);
  Serial.println(" bytes");

  if (lastLba < 128 || blockLength == 0 ||
      blockLength > MSD_MAX_LOGICAL_BLOCK_SIZE || blockLength > 0xFFFFUL) {
    Serial.println("STOP: unsupported/too-small medium or logical block size; no write attempted.");
    return;
  }

  // Select a scratch location away from the usual front-of-disk partition
  // metadata. This is still a raw-sector write: only use a blank test medium.
  const uint32_t testLba = lastLba / 2UL;
  Serial.print("[WRITE TEST] Scratch LBA: ");
  Serial.println((unsigned long)testLba);
  Serial.println("[WRITE TEST] No partition/filesystem metadata is intentionally targeted.");

  uint8_t writeData[MSD_MAX_LOGICAL_BLOCK_SIZE] = {0};
  uint8_t readBack[MSD_MAX_LOGICAL_BLOCK_SIZE] = {0};
  static const uint8_t marker[] = "SL811HS-WRITE-VERIFY-V1";
  for (uint32_t i = 0; i < blockLength; i++)
    writeData[i] = (uint8_t)(((i * 73UL) + (i >> 1) + 0x5DUL) ^ (i >> 3));
  for (uint8_t i = 0; i < sizeof(marker) - 1 && i < blockLength; i++)
    writeData[i] = marker[i];

  // WRITE(10): opcode 0x2A, one logical block, data flows host -> device.
  uint8_t write10Cdb[10] = {
    0x2A, 0x00,
    (uint8_t)(testLba >> 24),
    (uint8_t)(testLba >> 16),
    (uint8_t)(testLba >> 8),
    (uint8_t)testLba,
    0x00,
    0x00, 0x01,
    0x00
  };

  Serial.println();
  Serial.println("[1/3] WRITE(10) - sending one patterned logical block");
  if (!msdBotExecuteScsi(bot, lun, "WRITE(10) test block", write10Cdb,
                         sizeof(write10Cdb), blockLength, false,
                         writeData, sizeof(writeData),
                         bulkOutData1, bulkInData1, nextTag++, actual, status)) {
    Serial.println("STOP: WRITE(10) transport failed.");
    Serial.println("WARNING: The target sector may have been partially or fully modified.");
    Serial.println("Do not treat this device as unchanged; inspect/reinitialize it before reuse.");
    return;
  }
  if (status != 0x00 || actual != blockLength) {
    if (status == 0x01)
      msdRequestSenseAndPrint(bot, lun, bulkOutData1, bulkInData1, nextTag);
    Serial.println("STOP: WRITE(10) did not pass; the target sector may have changed.");
    return;
  }
  Serial.println("[WRITE TEST] WRITE(10) returned GOOD with zero residue.");

  // READ(10) the very same LBA and compare every byte with the sent pattern.
  uint8_t read10Cdb[10] = {
    0x28, 0x00,
    (uint8_t)(testLba >> 24),
    (uint8_t)(testLba >> 16),
    (uint8_t)(testLba >> 8),
    (uint8_t)testLba,
    0x00,
    0x00, 0x01,
    0x00
  };

  Serial.println();
  Serial.println("[2/3] READ(10) - reading the same scratch block back");
  if (!msdBotExecuteScsi(bot, lun, "READ(10) write verification", read10Cdb,
                         sizeof(read10Cdb), blockLength, true,
                         readBack, sizeof(readBack),
                         bulkOutData1, bulkInData1, nextTag++, actual, status)) {
    Serial.println("STOP: read-back transport failed after the write.");
    Serial.println("The test block remains modified; do not assume verification succeeded.");
    return;
  }
  if (status != 0x00 || actual != blockLength) {
    if (status == 0x01)
      msdRequestSenseAndPrint(bot, lun, bulkOutData1, bulkInData1, nextTag);
    Serial.println("STOP: read-back command failed or returned a short block.");
    Serial.println("The test block remains modified; do not assume verification succeeded.");
    return;
  }

  uint32_t mismatchCount = 0;
  uint32_t firstMismatch = 0xFFFFFFFFUL;
  for (uint32_t i = 0; i < blockLength; i++) {
    if (readBack[i] != writeData[i]) {
      if (firstMismatch == 0xFFFFFFFFUL)
        firstMismatch = i;
      mismatchCount++;
    }
  }

  Serial.println();
  Serial.println("[3/3] BYTE-BY-BYTE COMPARISON");
  Serial.print("Compared bytes: ");
  Serial.println((unsigned long)blockLength);
  Serial.print("Mismatched bytes: ");
  Serial.println((unsigned long)mismatchCount);
  if (mismatchCount == 0) {
    Serial.println("WRITE/READ-BACK VERIFY: PASS - every byte matches.");
    Serial.println("The scratch block intentionally retains the test pattern.");
  } else {
    Serial.print("First mismatch offset: 0x");
    printHexByte((uint8_t)(firstMismatch >> 8));
    printHexByte((uint8_t)firstMismatch);
    Serial.println();
    Serial.println("WRITE/READ-BACK VERIFY: FAIL.");
  }

  Serial.println();
  Serial.println("========================================");
  Serial.println(mismatchCount == 0 ? " SCSI WRITE TEST PASSED" : " SCSI WRITE TEST FAILED");
  Serial.print("Scratch LBA: ");
  Serial.println((unsigned long)testLba);
  Serial.println("One logical block was written; no filesystem formatting was performed.");
  Serial.println("========================================");
}


// ==========================================
// AMIGA RDB SEARCH - FIRST 16 LOGICAL BLOCKS
// ==========================================
#define MSD_RDB_SCAN_BLOCKS 16
#define MSD_RDB_SIGNATURE   0x5244534BUL  // ASCII "RDSK"
#define MSD_RDB_TAG_BASE    0x32424452UL  // little-endian bytes spell "RDB2"

void poseidonMassStorageFindRdb() {
  Serial.println();
  Serial.println("========================================");
  Serial.println(" AMIGA RIGID DISK BLOCK (RDB) SCAN");
  Serial.println(" SCSI READ(10), LBA 0..15");
  Serial.println("========================================");
  Serial.println("READ-ONLY operation. No write commands are issued.");
  Serial.println("Search rule: RDB signature 'RDSK' at byte offset 0 of a logical block.");

  if (!enumAddress1Ready) {
    Serial.println("ERROR: Run POSEIDON, then ENUM18 successfully first.");
    return;
  }

  MsdBotInterface bot;
  if (!msdFindBotInterface(bot)) {
    Serial.println("STOP: no compatible Mass Storage BOT interface was found.");
    return;
  }

  char configLabel[64];
  snprintf(configLabel, sizeof(configLabel), "SET_CONFIGURATION(%u) - Amiga RDB scan",
           bot.configurationValue);
  if (!enumControlNoData(0x00, 0x09, bot.configurationValue, 0, configLabel)) {
    Serial.println("STOP: SET_CONFIGURATION failed.");
    return;
  }

  uint8_t maxLun = 0;
  if (!msdGetMaxLun(bot.interfaceNumber, maxLun)) {
    Serial.println("STOP: GET_MAX_LUN failed.");
    return;
  }
  Serial.println("Scanning LUN 0 only.");
  const uint8_t lun = 0;

  // Keep each BOT pipe's DATA toggle across all commands in this session.
  bool bulkOutData1 = false;
  bool bulkInData1 = false;
  uint32_t nextTag = MSD_RDB_TAG_BASE;
  uint16_t actual = 0;
  uint8_t status = 0xFF;

  // First clear a possible medium-change Unit Attention.
  const uint8_t turCdb[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  bool mediumReady = false;
  uint8_t uaRetries = 0;
  const uint8_t maxUaRetries = 2;
  Serial.println();
  Serial.println("[RDB] TEST UNIT READY");
  while (!mediumReady) {
    if (!msdBotExecuteScsi(bot, lun, "TEST UNIT READY",
                           turCdb, sizeof(turCdb), 0, false, NULL, 0,
                           bulkOutData1, bulkInData1, nextTag++, actual, status)) {
      Serial.println("STOP: TEST UNIT READY transport failed.");
      return;
    }
    if (status == 0x00) {
      mediumReady = true;
      Serial.println("[RDB] Medium is ready.");
      break;
    }
    if (status != 0x01) {
      Serial.print("STOP: TEST UNIT READY returned SCSI status 0x");
      printHexByte(status);
      Serial.println();
      return;
    }

    uint8_t senseKey = 0xFF, asc = 0xFF, ascq = 0xFF;
    if (!msdRequestSenseAndPrint(bot, lun, bulkOutData1, bulkInData1,
                                 nextTag, &senseKey, &asc, &ascq)) {
      Serial.println("STOP: REQUEST SENSE failed; medium readiness is unknown.");
      return;
    }
    if (senseKey != 0x06 || asc != 0x28 || ascq != 0x00) {
      Serial.println("STOP: CHECK CONDITION is not medium-change UNIT ATTENTION 06/28/00.");
      return;
    }
    if (uaRetries >= maxUaRetries) {
      Serial.println("STOP: UNIT ATTENTION persisted after the retry limit.");
      return;
    }
    uaRetries++;
    Serial.print("[RDB] Unit Attention acknowledged; retrying TUR (");
    Serial.print(uaRetries);
    Serial.print("/");
    Serial.print(maxUaRetries);
    Serial.println(").");
    delay(20);
  }

  // Determine the device's logical block size and ensure LBA 0..15 exist.
  const uint8_t capacityCdb[10] = {
    0x25, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
  };
  uint8_t capacityData[8] = {0};
  Serial.println();
  Serial.println("[RDB] READ CAPACITY (10)");
  if (!msdBotExecuteScsi(bot, lun, "READ CAPACITY (10)",
                         capacityCdb, sizeof(capacityCdb),
                         sizeof(capacityData), true,
                         capacityData, sizeof(capacityData),
                         bulkOutData1, bulkInData1, nextTag++, actual, status)) {
    Serial.println("STOP: READ CAPACITY transport failed.");
    return;
  }
  if (status != 0x00) {
    if (status == 0x01)
      msdRequestSenseAndPrint(bot, lun, bulkOutData1, bulkInData1, nextTag);
    Serial.println("STOP: READ CAPACITY did not pass.");
    return;
  }
  if (actual != sizeof(capacityData)) {
    Serial.println("STOP: READ CAPACITY did not return exactly 8 bytes.");
    return;
  }

  uint32_t lastLba = msdReadBe32(&capacityData[0]);
  uint32_t blockLength = msdReadBe32(&capacityData[4]);
  Serial.print("[RDB] Last LBA: ");
  Serial.println((unsigned long)lastLba);
  Serial.print("[RDB] Logical block size: ");
  Serial.print((unsigned long)blockLength);
  Serial.println(" bytes");

  // The RDB format carries its own block-byte size. Amiga filesystems
  // support 256-byte and larger logical blocks; keep this diagnostic bounded
  // to the tester's existing maximum block buffer.
  if (blockLength < 256 || blockLength > MSD_MAX_LOGICAL_BLOCK_SIZE ||
      (blockLength & 3) != 0) {
    Serial.print("STOP: unsupported logical block size for RDB scan: ");
    Serial.println((unsigned long)blockLength);
    Serial.println("Expected 256..4096 bytes, divisible by four.");
    return;
  }
  uint32_t finalLba = (lastLba < (MSD_RDB_SCAN_BLOCKS - 1))
                    ? lastLba : (MSD_RDB_SCAN_BLOCKS - 1);
  Serial.print("[RDB] Scanning logical blocks 0 through ");
  Serial.println((unsigned long)finalLba);

  uint8_t block[MSD_MAX_LOGICAL_BLOCK_SIZE];
  bool foundRdb = false;
  bool sawRdbSignature = false;
  uint32_t foundLba = 0;
  uint32_t foundSummedLongs = 0;
  uint32_t foundChecksumSum = 0;

  for (uint32_t lba = 0; lba <= finalLba; lba++) {
    uint8_t readCdb[10] = {
      0x28, 0x00,             // READ(10)
      0x00, 0x00, 0x00, 0x00, // LBA filled below, big-endian
      0x00,                   // group number
      0x00, 0x01,             // one logical block
      0x00                    // control
    };
    readCdb[2] = (uint8_t)(lba >> 24);
    readCdb[3] = (uint8_t)(lba >> 16);
    readCdb[4] = (uint8_t)(lba >> 8);
    readCdb[5] = (uint8_t)lba;

    char commandName[40];
    snprintf(commandName, sizeof(commandName), "RDB SCAN READ(10) LBA %lu",
             (unsigned long)lba);
    Serial.println();
    Serial.print("[RDB] Reading LBA ");
    Serial.println((unsigned long)lba);

    if (!msdBotExecuteScsi(bot, lun, commandName,
                           readCdb, sizeof(readCdb),
                           (uint16_t)blockLength, true, block, sizeof(block),
                           bulkOutData1, bulkInData1, nextTag++, actual, status)) {
      Serial.print("STOP: transport failure reading LBA ");
      Serial.println((unsigned long)lba);
      return;
    }
    if (status != 0x00) {
      if (status == 0x01)
        msdRequestSenseAndPrint(bot, lun, bulkOutData1, bulkInData1, nextTag);
      Serial.print("STOP: SCSI READ(10) failed at LBA ");
      Serial.println((unsigned long)lba);
      return;
    }
    if (actual != blockLength) {
      Serial.print("STOP: short read at LBA ");
      Serial.print((unsigned long)lba);
      Serial.print("; expected ");
      Serial.print((unsigned long)blockLength);
      Serial.print(", received ");
      Serial.println(actual);
      return;
    }

    if (msdReadBe32(block) == MSD_RDB_SIGNATURE) {
      sawRdbSignature = true;
      uint32_t candidateSummedLongs = msdReadBe32(&block[4]);
      if (candidateSummedLongs < 3 || candidateSummedLongs > (blockLength / 4)) {
        Serial.print("[RDB] RDSK signature at LBA ");
        Serial.print((unsigned long)lba);
        Serial.println(" but rdb_SummedLongs is invalid; continuing scan.");
        continue;
      }

      uint32_t candidateChecksumSum = 0;
      for (uint32_t i = 0; i < candidateSummedLongs; i++)
        candidateChecksumSum += msdReadBe32(&block[i * 4]);

      if (candidateChecksumSum != 0) {
        Serial.print("[RDB] RDSK signature at LBA ");
        Serial.print((unsigned long)lba);
        Serial.println(" but checksum is not zero; continuing scan.");
        continue;
      }

      foundRdb = true;
      foundLba = lba;
      foundSummedLongs = candidateSummedLongs;
      foundChecksumSum = candidateChecksumSum;
      break;
    }
  }

  if (!foundRdb) {
    Serial.println();
    Serial.println("========================================");
    if (sawRdbSignature)
      Serial.println(" RDSK SIGNATURE FOUND, BUT NO VALID RDB CHECKSUM");
    else
      Serial.println(" NO RDB SIGNATURE FOUND");
    Serial.print("Scanned ");
    Serial.print((unsigned long)(finalLba + 1));
    Serial.println(" logical blocks.");
    Serial.println("A valid RDB needs the RDSK identifier and a sum-to-zero checksum.");
    Serial.println("The RDB may be absent or outside this scan window.");
    Serial.println("No media writes were issued.");
    Serial.println("========================================");
    return;
  }

  Serial.println();
  Serial.println("========================================");
  Serial.println(" AMIGA RDB SIGNATURE FOUND!");
  Serial.print("RDB block LBA: ");
  Serial.println((unsigned long)foundLba);

  Serial.print("RDB summed longs: ");
  Serial.println((unsigned long)foundSummedLongs);
  Serial.print("RDB checksum sum: 0x");
  for (int8_t shift = 24; shift >= 0; shift -= 8)
    printHexByte((uint8_t)(foundChecksumSum >> shift));
  Serial.println(" (valid / sum-to-zero)");
  uint32_t rdbBlockBytes = msdReadBe32(&block[16]);
  Serial.print("RDB block bytes: ");
  Serial.println((unsigned long)rdbBlockBytes);
  if (rdbBlockBytes != blockLength)
    Serial.println("WARNING: rdb_BlockBytes differs from SCSI logical block size.");
  Serial.print("BadBlockList block: ");
  Serial.println((unsigned long)msdReadBe32(&block[24]));
  Serial.print("PartitionList block: ");
  Serial.println((unsigned long)msdReadBe32(&block[28]));
  Serial.print("FileSysHeaderList block: ");
  Serial.println((unsigned long)msdReadBe32(&block[32]));
  Serial.print("Cylinders: ");
  Serial.println((unsigned long)msdReadBe32(&block[64]));
  Serial.print("Sectors per track: ");
  Serial.println((unsigned long)msdReadBe32(&block[68]));
  Serial.print("Heads: ");
  Serial.println((unsigned long)msdReadBe32(&block[72]));
  Serial.println("========================================");

  msdPrintSectorHexDump(block, (uint16_t)blockLength, foundLba);
  Serial.println();
  Serial.println("RDB scan completed. All commands were read-only.");
}

// ==========================================
// AMIGA RDB FOLLOW-UP: PART + FSHD LINKED LISTS
// ==========================================
#define MSD_PART_SIGNATURE 0x50415254UL  // "PART"
#define MSD_FSHD_SIGNATURE 0x46534844UL  // "FSHD"
#define MSD_RDB_LIST_LIMIT 16
#define MSD_PART_ENV_OFFSET 128

void msdRdbPrintHex32(uint32_t value) {
  Serial.print("0x");
  for (int8_t shift = 24; shift >= 0; shift -= 8)
    printHexByte((uint8_t)(value >> shift));
}

void msdRdbPrintFourCC(const char *label, uint32_t value) {
  Serial.print(label);
  Serial.print("0x");
  for (int8_t shift = 24; shift >= 0; shift -= 8)
    printHexByte((uint8_t)(value >> shift));
  Serial.print(" ('");
  for (int8_t shift = 24; shift >= 0; shift -= 8) {
    uint8_t c = (uint8_t)(value >> shift);
    Serial.print((c >= 0x20 && c <= 0x7E) ? (char)c : '.');
  }
  Serial.println("')");
}

bool msdRdbValidateBlock(const uint8_t *block,
                         uint32_t blockLength,
                         uint32_t expectedSignature,
                         const char *blockType,
                         uint32_t &summedLongs,
                         uint32_t &checksumSum) {
  summedLongs = 0;
  checksumSum = 0;

  if (blockLength < 12 || (blockLength & 3) != 0) {
    Serial.print("[RDB] ");
    Serial.print(blockType);
    Serial.println(" block has an unsupported length.");
    return false;
  }

  uint32_t signature = msdReadBe32(&block[0]);
  if (signature != expectedSignature) {
    Serial.print("[RDB] Expected ");
    Serial.print(blockType);
    Serial.print(" but found ");
    msdRdbPrintFourCC("signature: ", signature);
    return false;
  }

  summedLongs = msdReadBe32(&block[4]);
  if (summedLongs < 3 || summedLongs > (blockLength / 4)) {
    Serial.print("[RDB] ");
    Serial.print(blockType);
    Serial.print(" has invalid SummedLongs=");
    Serial.println((unsigned long)summedLongs);
    return false;
  }

  for (uint32_t i = 0; i < summedLongs; i++)
    checksumSum += msdReadBe32(&block[i * 4]);

  if (checksumSum != 0) {
    Serial.print("[RDB] ");
    Serial.print(blockType);
    Serial.print(" checksum INVALID: ");
    msdRdbPrintHex32(checksumSum);
    Serial.println();
    return false;
  }
  return true;
}

bool msdRdbReadBlock(const MsdBotInterface &bot,
                     uint8_t lun,
                     uint32_t lba,
                     uint32_t lastLba,
                     uint32_t blockLength,
                     uint8_t *block,
                     bool &bulkOutData1,
                     bool &bulkInData1,
                     uint32_t &nextTag,
                     const char *purpose) {
  if (lba > lastLba) {
    Serial.print("[RDB] Refusing out-of-range block address ");
    Serial.println((unsigned long)lba);
    return false;
  }
  if (blockLength == 0 || blockLength > MSD_MAX_LOGICAL_BLOCK_SIZE ||
      blockLength > 0xFFFFUL) {
    Serial.println("[RDB] Unsupported block length for READ(10).");
    return false;
  }

  uint8_t cdb[10] = {
    0x28, 0x00,
    (uint8_t)(lba >> 24), (uint8_t)(lba >> 16),
    (uint8_t)(lba >> 8), (uint8_t)lba,
    0x00, 0x00, 0x01, 0x00
  };
  char commandName[56];
  snprintf(commandName, sizeof(commandName), "%s LBA %lu",
           purpose, (unsigned long)lba);

  uint16_t actual = 0;
  uint8_t status = 0xFF;
  if (!msdBotExecuteScsi(bot, lun, commandName,
                         cdb, sizeof(cdb), blockLength, true,
                         block, (uint16_t)MSD_MAX_LOGICAL_BLOCK_SIZE,
                         bulkOutData1, bulkInData1, nextTag++, actual, status)) {
    Serial.print("[RDB] Transport failed reading LBA ");
    Serial.println((unsigned long)lba);
    return false;
  }
  if (status != 0x00) {
    if (status == 0x01)
      msdRequestSenseAndPrint(bot, lun, bulkOutData1, bulkInData1, nextTag);
    Serial.print("[RDB] SCSI READ(10) failed at LBA ");
    Serial.println((unsigned long)lba);
    return false;
  }
  if (actual != blockLength) {
    Serial.print("[RDB] Short read at LBA ");
    Serial.print((unsigned long)lba);
    Serial.print(": expected ");
    Serial.print((unsigned long)blockLength);
    Serial.print(", received ");
    Serial.println(actual);
    return false;
  }
  return true;
}

void msdRdbPrintBstrName(const uint8_t *block) {
  uint8_t nameLength = block[36];
  if (nameLength > 31) nameLength = 31;
  Serial.print("  Drive name: \"");
  for (uint8_t i = 0; i < nameLength; i++) {
    uint8_t c = block[37 + i];
    Serial.print((c >= 0x20 && c <= 0x7E) ? (char)c : '.');
  }
  Serial.println("\"");
}

void msdRdbPrintDosType(const char *label, uint32_t dosType) {
  msdRdbPrintFourCC(label, dosType);
}

void msdRdbPrintPartition(const uint8_t *block,
                          uint32_t blockLength,
                          uint32_t lastLba,
                          uint8_t index) {
  uint32_t flags = msdReadBe32(&block[20]);
  uint32_t next = msdReadBe32(&block[16]);

  Serial.println();
  Serial.print("[PART] Partition entry ");
  Serial.println(index + 1);
  Serial.print("  Next PART block: ");
  Serial.println((unsigned long)next);
  Serial.print("  Flags: ");
  msdRdbPrintHex32(flags);
  Serial.println();
  Serial.print("  Bootable: ");
  Serial.println((flags & 0x01) ? "YES" : "NO");
  Serial.print("  Do not mount: ");
  Serial.println((flags & 0x02) ? "YES" : "NO");
  msdRdbPrintBstrName(block);
  Serial.print("  Device flags: ");
  msdRdbPrintHex32(msdReadBe32(&block[32]));
  Serial.println();

  if (blockLength < (MSD_PART_ENV_OFFSET + 17UL * 4UL)) {
    Serial.println("  Environment vector is truncated by the logical block size.");
    return;
  }

  const uint8_t *env = &block[MSD_PART_ENV_OFFSET];
  uint32_t tableSize = msdReadBe32(&env[0]);
  Serial.print("  DosEnvec TableSize: ");
  Serial.println((unsigned long)tableSize);
  // TableSize counts longwords following de_TableSize. Geometry through
  // de_HighCyl is available when it is at least 10; later fields are optional.
  if (tableSize < 10) {
    Serial.println("  DosEnvec is too short to contain the cylinder geometry.");
    return;
  }

  uint32_t sizeBlock = msdReadBe32(&env[4]);
  uint32_t secOrg = msdReadBe32(&env[8]);
  uint32_t surfaces = msdReadBe32(&env[12]);
  uint32_t sectorsPerBlock = msdReadBe32(&env[16]);
  uint32_t blocksPerTrack = msdReadBe32(&env[20]);
  uint32_t reservedBlocks = msdReadBe32(&env[24]);
  uint32_t preAlloc = msdReadBe32(&env[28]);
  uint32_t interleave = msdReadBe32(&env[32]);
  uint32_t lowCyl = msdReadBe32(&env[36]);
  uint32_t highCyl = msdReadBe32(&env[40]);

  Serial.println("  DosEnvec:");
  Serial.print("    SizeBlock (longwords): ");
  Serial.println((unsigned long)sizeBlock);
  Serial.print("    SecOrg: ");
  Serial.println((unsigned long)secOrg);
  Serial.print("    Surfaces: ");
  Serial.println((unsigned long)surfaces);
  Serial.print("    SectorPerBlock: ");
  Serial.println((unsigned long)sectorsPerBlock);
  Serial.print("    BlocksPerTrack: ");
  Serial.println((unsigned long)blocksPerTrack);
  Serial.print("    Reserved blocks: ");
  Serial.println((unsigned long)reservedBlocks);
  Serial.print("    PreAlloc: ");
  Serial.println((unsigned long)preAlloc);
  Serial.print("    Interleave: ");
  Serial.println((unsigned long)interleave);
  Serial.print("    LowCyl: ");
  Serial.println((unsigned long)lowCyl);
  Serial.print("    HighCyl: ");
  Serial.println((unsigned long)highCyl);
  if (tableSize >= 11) {
    Serial.print("    NumBuffers: ");
    Serial.println((unsigned long)msdReadBe32(&env[44]));
  } else {
    Serial.println("    NumBuffers: not present in this DosEnvec version");
  }
  if (tableSize >= 15) {
    Serial.print("    BootPri: ");
    Serial.println((long)(int32_t)msdReadBe32(&env[60]));
  } else {
    Serial.println("    BootPri: not present in this DosEnvec version");
  }
  if (tableSize >= 16) {
    msdRdbPrintDosType("    DosType: ", msdReadBe32(&env[64]));
  } else {
    Serial.println("    DosType: not present in this DosEnvec version");
  }
  uint64_t fsBlockBytes = (uint64_t)sizeBlock * 4ULL * sectorsPerBlock;
  Serial.print("    Filesystem logical block bytes: ");
  msdPrintUint64(fsBlockBytes);
  Serial.println();

  // Convert the partition's cylinder geometry to device LBAs only for
  // conventional 512-byte media and sane non-zero geometry values.
  if (surfaces != 0 && blocksPerTrack != 0 && highCyl >= lowCyl) {
    uint64_t blocksPerCylinder = (uint64_t)surfaces * blocksPerTrack;
    uint64_t startLba = (uint64_t)lowCyl * blocksPerCylinder;
    uint64_t endExclusive = ((uint64_t)highCyl + 1ULL) * blocksPerCylinder;
    if (endExclusive > 0 && endExclusive - 1ULL <= lastLba) {
      Serial.print("  Calculated start LBA: ");
      msdPrintUint64(startLba);
      Serial.println();
      Serial.print("  Calculated end LBA: ");
      msdPrintUint64(endExclusive - 1ULL);
      Serial.println();
      Serial.print("  Calculated partition blocks: ");
      msdPrintUint64(endExclusive - startLba);
      Serial.println();
    } else {
      Serial.println("  Cylinder-derived LBA range exceeds reported device capacity; not trusted.");
    }
  } else {
    Serial.println("  Cannot calculate LBA range from invalid cylinder geometry.");
  }
}

void msdRdbPrintFileSysHeader(const uint8_t *block, uint8_t index) {
  Serial.println();
  Serial.print("[FSHD] Filesystem header entry ");
  Serial.println(index + 1);
  Serial.print("  Next FSHD block: ");
  Serial.println((unsigned long)msdReadBe32(&block[16]));
  Serial.print("  Flags: ");
  msdRdbPrintHex32(msdReadBe32(&block[20]));
  Serial.println();
  msdRdbPrintDosType("  DosType: ", msdReadBe32(&block[32]));
  Serial.print("  Version raw: ");
  msdRdbPrintHex32(msdReadBe32(&block[36]));
  Serial.println();
  Serial.print("  PatchFlags: ");
  msdRdbPrintHex32(msdReadBe32(&block[40]));
  Serial.println();
  Serial.print("  Device node Type: ");
  msdRdbPrintHex32(msdReadBe32(&block[44]));
  Serial.println();
  Serial.print("  StackSize: ");
  Serial.println((unsigned long)msdReadBe32(&block[60]));
  Serial.print("  Priority: ");
  Serial.println((long)(int32_t)msdReadBe32(&block[64]));
  Serial.print("  SegListBlocks (first LSEG): ");
  Serial.println((unsigned long)msdReadBe32(&block[72]));
  Serial.print("  GlobalVec: ");
  msdRdbPrintHex32(msdReadBe32(&block[76]));
  Serial.println();
}

bool msdRdbReadAndValidateListBlock(const MsdBotInterface &bot,
                                   uint8_t lun,
                                   uint32_t lba,
                                   uint32_t lastLba,
                                   uint32_t blockLength,
                                   uint32_t expectedSignature,
                                   const char *blockType,
                                   uint8_t *block,
                                   bool &bulkOutData1,
                                   bool &bulkInData1,
                                   uint32_t &nextTag,
                                   uint32_t &nextBlock) {
  nextBlock = 0xFFFFFFFFUL;
  if (!msdRdbReadBlock(bot, lun, lba, lastLba, blockLength, block,
                       bulkOutData1, bulkInData1, nextTag, blockType))
    return false;

  uint32_t summedLongs = 0;
  uint32_t checksumSum = 0;
  if (!msdRdbValidateBlock(block, blockLength, expectedSignature,
                           blockType, summedLongs, checksumSum))
    return false;

  Serial.print("  ");
  Serial.print(blockType);
  Serial.print(" checksum OK; SummedLongs=");
  Serial.println((unsigned long)summedLongs);
  nextBlock = msdReadBe32(&block[16]);
  return true;
}

void poseidonMassStorageRdbDetails() {
  Serial.println();
  Serial.println("========================================");
  Serial.println(" AMIGA RDB DETAILS - PART + FSHD LISTS");
  Serial.println(" READ-ONLY; no SCSI write commands");
  Serial.println("========================================");

  if (!enumAddress1Ready) {
    Serial.println("ERROR: Run POSEIDON, then ENUM18 successfully first.");
    return;
  }

  MsdBotInterface bot;
  if (!msdFindBotInterface(bot)) {
    Serial.println("STOP: no compatible Mass Storage BOT interface was found.");
    return;
  }

  char configLabel[64];
  snprintf(configLabel, sizeof(configLabel), "SET_CONFIGURATION(%u) - RDB details",
           bot.configurationValue);
  if (!enumControlNoData(0x00, 0x09, bot.configurationValue, 0, configLabel)) {
    Serial.println("STOP: SET_CONFIGURATION failed.");
    return;
  }

  uint8_t maxLun = 0;
  if (!msdGetMaxLun(bot.interfaceNumber, maxLun)) {
    Serial.println("STOP: GET_MAX_LUN failed.");
    return;
  }
  const uint8_t lun = 0;
  Serial.println("[RDB] Working on LUN 0.");

  bool bulkOutData1 = false;
  bool bulkInData1 = false;
  uint32_t nextTag = 0x32424460UL;
  uint16_t actual = 0;
  uint8_t status = 0xFF;

  // TEST UNIT READY; acknowledge only the known medium-change Unit Attention.
  const uint8_t turCdb[6] = {0x00, 0, 0, 0, 0, 0};
  bool ready = false;
  for (uint8_t attempt = 0; attempt < 3 && !ready; attempt++) {
    if (!msdBotExecuteScsi(bot, lun, "TEST UNIT READY", turCdb, sizeof(turCdb),
                           0, false, NULL, 0, bulkOutData1, bulkInData1,
                           nextTag++, actual, status)) {
      Serial.println("STOP: TEST UNIT READY transport failure.");
      return;
    }
    if (status == 0x00) {
      ready = true;
      break;
    }
    if (status != 0x01) {
      Serial.println("STOP: unexpected TEST UNIT READY SCSI status.");
      return;
    }
    uint8_t senseKey = 0xFF, asc = 0xFF, ascq = 0xFF;
    if (!msdRequestSenseAndPrint(bot, lun, bulkOutData1, bulkInData1,
                                 nextTag, &senseKey, &asc, &ascq)) {
      Serial.println("STOP: REQUEST SENSE failed.");
      return;
    }
    if (!(senseKey == 0x06 && asc == 0x28 && ascq == 0x00)) {
      Serial.println("STOP: unhandled SCSI CHECK CONDITION.");
      return;
    }
    Serial.println("[RDB] Unit Attention acknowledged; retrying TEST UNIT READY.");
  }
  if (!ready) {
    Serial.println("STOP: medium did not become ready after Unit Attention retries.");
    return;
  }
  Serial.println("[RDB] Medium ready.");

  const uint8_t capacityCdb[10] = {0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  uint8_t capacityData[8] = {0};
  if (!msdBotExecuteScsi(bot, lun, "READ CAPACITY (10)", capacityCdb,
                         sizeof(capacityCdb), sizeof(capacityData), true,
                         capacityData, sizeof(capacityData), bulkOutData1,
                         bulkInData1, nextTag++, actual, status) ||
      status != 0x00 || actual != sizeof(capacityData)) {
    Serial.println("STOP: READ CAPACITY (10) failed or returned incomplete data.");
    return;
  }

  uint32_t lastLba = msdReadBe32(&capacityData[0]);
  uint32_t blockLength = msdReadBe32(&capacityData[4]);
  if (blockLength < 256 || blockLength > MSD_MAX_LOGICAL_BLOCK_SIZE ||
      (blockLength & 3) != 0) {
    Serial.println("STOP: unsupported logical block size for RDB structures.");
    return;
  }
  Serial.print("[RDB] Device last LBA: ");
  Serial.println((unsigned long)lastLba);
  Serial.print("[RDB] Logical block size: ");
  Serial.println((unsigned long)blockLength);

  uint8_t block[MSD_MAX_LOGICAL_BLOCK_SIZE];
  bool foundRdb = false;
  uint32_t rdbLba = 0;
  uint32_t partitionHead = 0xFFFFFFFFUL;
  uint32_t fileSysHead = 0xFFFFFFFFUL;

  Serial.println("[RDB] Searching blocks 0..15 for a checksum-valid RDSK.");
  uint32_t scanEnd = (lastLba < 15) ? lastLba : 15;
  for (uint32_t lba = 0; lba <= scanEnd; lba++) {
    if (!msdRdbReadBlock(bot, lun, lba, lastLba, blockLength, block,
                         bulkOutData1, bulkInData1, nextTag, "RDSK SEARCH"))
      return;
    if (msdReadBe32(block) != MSD_RDB_SIGNATURE)
      continue;

    uint32_t summedLongs = 0, checksumSum = 0;
    if (!msdRdbValidateBlock(block, blockLength, MSD_RDB_SIGNATURE,
                             "RDSK", summedLongs, checksumSum))
      continue;
    foundRdb = true;
    rdbLba = lba;
    uint32_t rdbBlockBytes = msdReadBe32(&block[16]);
    if (rdbBlockBytes != blockLength) {
      Serial.print("[RDB] RDSK block size ");
      Serial.print((unsigned long)rdbBlockBytes);
      Serial.print(" differs from SCSI logical block size ");
      Serial.println((unsigned long)blockLength);
      Serial.println("STOP: refusing to follow block pointers with mismatched block sizes.");
      return;
    }
    partitionHead = msdReadBe32(&block[28]);
    fileSysHead = msdReadBe32(&block[32]);
    Serial.print("[RDB] Valid RDSK at LBA ");
    Serial.println((unsigned long)rdbLba);
    Serial.print("[RDB] Block bytes: ");
    Serial.println((unsigned long)rdbBlockBytes);
    msdPrintAsciiField("[RDB] Disk vendor: ", block, 160, 8);
    msdPrintAsciiField("[RDB] Disk product: ", block, 168, 16);
    msdPrintAsciiField("[RDB] Disk revision: ", block, 184, 4);
    Serial.print("[RDB] PartitionList head: ");
    Serial.println((unsigned long)partitionHead);
    Serial.print("[RDB] FileSysHeaderList head: ");
    Serial.println((unsigned long)fileSysHead);
    break;
  }
  if (!foundRdb) {
    Serial.println("STOP: no checksum-valid RDSK found in blocks 0..15.");
    return;
  }

  if (msdReadBe32(block) != MSD_RDB_SIGNATURE || rdbLba > lastLba) {
    if (!msdRdbReadBlock(bot, lun, rdbLba, lastLba, blockLength, block,
                         bulkOutData1, bulkInData1, nextTag, "RDSK RE-READ")) {
      Serial.println("STOP: unable to reload RDSK block.");
      return;
    }
    partitionHead = msdReadBe32(&block[28]);
    fileSysHead = msdReadBe32(&block[32]);
  }

  // Traverse linked PART blocks. A visited list prevents corrupted cycles.
  Serial.println();
  Serial.println("========================================");
  Serial.println(" PARTITION BLOCK LIST");
  Serial.println("========================================");
  uint32_t visitedPart[MSD_RDB_LIST_LIMIT];
  uint8_t partCount = 0;
  uint32_t current = partitionHead;
  while (current != 0xFFFFFFFFUL && partCount < MSD_RDB_LIST_LIMIT) {
    bool repeated = false;
    for (uint8_t i = 0; i < partCount; i++)
      if (visitedPart[i] == current) repeated = true;
    if (repeated) {
      Serial.print("STOP PART: linked-list cycle detected at LBA ");
      Serial.println((unsigned long)current);
      break;
    }
    if (current > lastLba) {
      Serial.print("STOP PART: block pointer outside device at LBA ");
      Serial.println((unsigned long)current);
      break;
    }
    visitedPart[partCount] = current;
    uint32_t nextBlock = 0xFFFFFFFFUL;
    if (!msdRdbReadAndValidateListBlock(bot, lun, current, lastLba,
                                        blockLength, MSD_PART_SIGNATURE, "PART",
                                        block, bulkOutData1, bulkInData1,
                                        nextTag, nextBlock)) {
      Serial.println("STOP: PART block invalid or unreadable.");
      break;
    }
    msdRdbPrintPartition(block, blockLength, lastLba, partCount);
    partCount++;
    current = nextBlock;
  }
  if (partCount == MSD_RDB_LIST_LIMIT && current != 0xFFFFFFFFUL)
    Serial.println("PART list stopped at safety limit (possible unusually long list).");
  if (partCount == 0 && partitionHead == 0xFFFFFFFFUL)
    Serial.println("No PART blocks are referenced by this RDSK.");

  // Traverse linked FSHD blocks. LSEG payload loading is intentionally out of scope.
  Serial.println();
  Serial.println("========================================");
  Serial.println(" FILESYSTEM HEADER BLOCK LIST");
  Serial.println("========================================");
  uint32_t visitedFshd[MSD_RDB_LIST_LIMIT];
  uint8_t fshdCount = 0;
  current = fileSysHead;
  while (current != 0xFFFFFFFFUL && fshdCount < MSD_RDB_LIST_LIMIT) {
    bool repeated = false;
    for (uint8_t i = 0; i < fshdCount; i++)
      if (visitedFshd[i] == current) repeated = true;
    if (repeated) {
      Serial.print("STOP FSHD: linked-list cycle detected at LBA ");
      Serial.println((unsigned long)current);
      break;
    }
    if (current > lastLba) {
      Serial.print("STOP FSHD: block pointer outside device at LBA ");
      Serial.println((unsigned long)current);
      break;
    }
    visitedFshd[fshdCount] = current;
    uint32_t nextBlock = 0xFFFFFFFFUL;
    if (!msdRdbReadAndValidateListBlock(bot, lun, current, lastLba,
                                        blockLength, MSD_FSHD_SIGNATURE, "FSHD",
                                        block, bulkOutData1, bulkInData1,
                                        nextTag, nextBlock)) {
      Serial.println("STOP: FSHD block invalid or unreadable.");
      break;
    }
    msdRdbPrintFileSysHeader(block, fshdCount);
    fshdCount++;
    current = nextBlock;
  }
  if (fshdCount == MSD_RDB_LIST_LIMIT && current != 0xFFFFFFFFUL)
    Serial.println("FSHD list stopped at safety limit (possible unusually long list).");
  if (fshdCount == 0 && fileSysHead == 0xFFFFFFFFUL)
    Serial.println("No FSHD blocks are referenced by this RDSK.");

  Serial.println();
  Serial.println("========================================");
  Serial.println(" RDB PART + FSHD INSPECTION FINISHED");
  Serial.print("Valid PART blocks read: ");
  Serial.println(partCount);
  Serial.print("Valid FSHD blocks read: ");
  Serial.println(fshdCount);
  Serial.println("All SCSI commands were read-only.");
  Serial.println("Filesystem LSEG payloads were not read.");
  Serial.println("========================================");
}

void poseidonMassStorageInquiry() {
  Serial.println();
  Serial.println("========================================");
  Serial.println(" USB MASS STORAGE - GENERIC BOT / SCSI INQUIRY");
  Serial.println("========================================");
  Serial.println("Read-only test. No media data is written.");

  if (!enumAddress1Ready) {
    Serial.println("ERROR: Run POSEIDON, then ENUM18 successfully first.");
    return;
  }

  uint16_t vid = (uint16_t)enumStoredDeviceDescriptor[8] |
                 ((uint16_t)enumStoredDeviceDescriptor[9] << 8);
  uint16_t pid = (uint16_t)enumStoredDeviceDescriptor[10] |
                 ((uint16_t)enumStoredDeviceDescriptor[11] << 8);
  Serial.print("Device VID:PID (informational only) = 0x");
  printHexByte((uint8_t)(vid >> 8));
  printHexByte((uint8_t)vid);
  Serial.print(":0x");
  printHexByte((uint8_t)(pid >> 8));
  printHexByte((uint8_t)pid);
  Serial.println();

  MsdBotInterface bot;
  if (!msdFindBotInterface(bot)) {
    Serial.println("STOP: no compatible Mass Storage BOT interface was found.");
    return;
  }

  // SET_CONFIGURATION resets the newly configured bulk pipes to DATA0.
  char configLabel[64];
  snprintf(configLabel, sizeof(configLabel), "SET_CONFIGURATION(%u) - discovered Mass Storage",
           (unsigned int)bot.configurationValue);
  if (!enumControlNoData(0x00, 0x09, bot.configurationValue, 0,
                         configLabel)) {
    Serial.println("STOP: SET_CONFIGURATION failed.");
    return;
  }

  uint8_t maxLun = 0;
  if (!msdGetMaxLun(bot.interfaceNumber, maxLun)) {
    Serial.println("STOP: GET_MAX_LUN failed; BOT command not started.");
    return;
  }
  if (maxLun != 0)
    Serial.println("NOTE: Device reports additional LUNs; this test uses LUN 0.");

  // Build the 31-byte Command Block Wrapper for SCSI INQUIRY (36 bytes).
  uint8_t cbw[MSD_CBW_LENGTH] = {0};
  cbw[0] = 0x55; cbw[1] = 0x53; cbw[2] = 0x42; cbw[3] = 0x43; // USBC
  cbw[4] = (uint8_t)(MSD_INQUIRY_TAG & 0xFF);
  cbw[5] = (uint8_t)((MSD_INQUIRY_TAG >> 8) & 0xFF);
  cbw[6] = (uint8_t)((MSD_INQUIRY_TAG >> 16) & 0xFF);
  cbw[7] = (uint8_t)((MSD_INQUIRY_TAG >> 24) & 0xFF);
  cbw[8] = MSD_INQUIRY_LENGTH; // dCBWDataTransferLength = 36, little-endian
  cbw[12] = 0x80;              // Data-In phase
  cbw[13] = 0x00;              // LUN 0
  cbw[14] = 0x06;              // 6-byte SCSI CDB
  cbw[15] = 0x12;              // INQUIRY
  cbw[16] = 0x00;              // EVPD=0, page code=0
  cbw[17] = 0x00;
  cbw[18] = 0x00;
  cbw[19] = MSD_INQUIRY_LENGTH; // allocation length = 36
  cbw[20] = 0x00;

  bool bulkOutData1 = false;
  bool bulkInData1 = false;

  Serial.println();
  Serial.println("[BOT 1/3] Send 31-byte CBW using discovered Bulk OUT pipe");
  if (!msdBulkOutBuffer(bot.bulkOutAddress, bot.bulkOutMaxPacket,
                        bulkOutData1, cbw, sizeof(cbw), "BOT CBW")) {
    Serial.println("STOP: CBW transfer failed. No data/CSW stage attempted.");
    return;
  }

  Serial.println();
  Serial.println("[BOT 2/3] Receive SCSI INQUIRY data using discovered Bulk IN pipe");
  uint8_t inquiry[MSD_INQUIRY_LENGTH] = {0};
  uint16_t inquiryReceived = 0;
  if (!msdBulkInBuffer(bot.bulkInAddress, bot.bulkInMaxPacket,
                       bulkInData1, inquiry, MSD_INQUIRY_LENGTH,
                       inquiryReceived, "BOT INQUIRY DATA")) {
    Serial.println("STOP: SCSI INQUIRY data stage failed.");
    return;
  }

  Serial.print("INQUIRY data bytes received: ");
  Serial.println(inquiryReceived);
  if (inquiryReceived == MSD_INQUIRY_LENGTH) {
    Serial.println("Raw INQUIRY response:");
    for (uint8_t i = 0; i < MSD_INQUIRY_LENGTH; i++) {
      printHexByte(inquiry[i]);
      if ((i & 0x0F) == 0x0F) Serial.println();
      else Serial.print(' ');
    }
    Serial.println();

    Serial.println("Decoded SCSI INQUIRY:");
    Serial.print("Peripheral qualifier: "); Serial.println((inquiry[0] >> 5) & 0x07);
    Serial.print("Peripheral device type: 0x"); printHexByte(inquiry[0] & 0x1F); Serial.println();
    Serial.print("Removable media: "); Serial.println((inquiry[1] & 0x80) ? "yes" : "no");
    Serial.print("SCSI version: 0x"); printHexByte(inquiry[2]); Serial.println();
    Serial.print("Response data format: 0x"); printHexByte(inquiry[3] & 0x0F); Serial.println();
    Serial.print("Additional length: "); Serial.println(inquiry[4]);
    msdPrintAsciiField("Vendor: ", inquiry, 8, 8);
    msdPrintAsciiField("Product: ", inquiry, 16, 16);
    msdPrintAsciiField("Revision: ", inquiry, 32, 4);
  } else {
    Serial.println("WARNING: INQUIRY returned a short response; CSW will still be checked.");
  }

  Serial.println();
  Serial.println("[BOT 3/3] Receive 13-byte CSW using the same Bulk IN pipe");
  Serial.print("Expected CSW DATA toggle: DATA");
  Serial.println(bulkInData1 ? 1 : 0);
  uint8_t csw[MSD_CSW_LENGTH] = {0};
  uint16_t cswReceived = 0;
  if (!msdBulkInBuffer(bot.bulkInAddress, bot.bulkInMaxPacket,
                       bulkInData1, csw, MSD_CSW_LENGTH,
                       cswReceived, "BOT CSW", MSD_CSW_TIMEOUT_MS)) {
    Serial.println("STOP: CSW transfer failed.");
    return;
  }

  Serial.print("CSW bytes received: ");
  Serial.println(cswReceived);
  if (cswReceived != MSD_CSW_LENGTH) {
    Serial.println("ERROR: CSW length is not 13 bytes.");
    return;
  }

  msdPrintLe32("CSW signature: ", &csw[0]);
  msdPrintLe32("CSW tag:       ", &csw[4]);
  msdPrintLe32("CSW residue:   ", &csw[8]);
  Serial.print("CSW status:    0x"); printHexByte(csw[12]); Serial.println();

  uint32_t cswSignature = (uint32_t)csw[0] |
                          ((uint32_t)csw[1] << 8) |
                          ((uint32_t)csw[2] << 16) |
                          ((uint32_t)csw[3] << 24);
  uint32_t cswTag = (uint32_t)csw[4] |
                    ((uint32_t)csw[5] << 8) |
                    ((uint32_t)csw[6] << 16) |
                    ((uint32_t)csw[7] << 24);
  uint32_t residue = (uint32_t)csw[8] |
                     ((uint32_t)csw[9] << 8) |
                     ((uint32_t)csw[10] << 16) |
                     ((uint32_t)csw[11] << 24);

  if (cswSignature != MSD_CSW_SIGNATURE) {
    Serial.println("ERROR: CSW signature mismatch (expected USBS / 0x53425355).");
    return;
  }
  if (cswTag != MSD_INQUIRY_TAG) {
    Serial.println("ERROR: CSW tag does not match the INQUIRY CBW.");
    return;
  }
  if (residue != (uint32_t)(MSD_INQUIRY_LENGTH - inquiryReceived)) {
    Serial.print("WARNING: CSW residue differs from the host-observed short transfer. Residue=");
    Serial.println(residue);
  }
  if (csw[12] == 0x00) {
    Serial.println("SCSI INQUIRY: COMMAND PASSED.");
  } else if (csw[12] == 0x01) {
    Serial.println("SCSI INQUIRY: COMMAND FAILED (CHECK CONDITION); next step will be REQUEST SENSE.");
    return;
  } else if (csw[12] == 0x02) {
    Serial.println("SCSI INQUIRY: PHASE ERROR; BOT reset recovery will be needed.");
    return;
  } else {
    Serial.println("ERROR: CSW has an invalid status value.");
    return;
  }

  Serial.println();
  Serial.println("========================================");
  Serial.println(" GENERIC BOT INQUIRY TEST PASSED");
  Serial.println(" Mass Storage interface/endpoints discovered from descriptors.");
  Serial.println(" CBW/data/CSW packetized using each endpoint's MPS.");
  Serial.println("========================================");
}

void enumPrintBootMouseReport(uint8_t reportNumber,
                              const uint8_t *data,
                              uint8_t length,
                              bool data1) {
  Serial.print("Report #");
  Serial.print(reportNumber);
  Serial.print("  DATA");
  Serial.print(data1 ? 1 : 0);
  Serial.print("  LEN=");
  Serial.println(length);

  Serial.print("  RAW:");
  for (uint8_t i = 0; i < length; i++) {
    Serial.print(' ');
    printHexByte(data[i]);
  }
  Serial.println();

  if (length >= 3) {
    uint8_t buttons = data[0];
    int8_t x = (int8_t)data[1];
    int8_t y = (int8_t)data[2];

    Serial.print("  Buttons: L="); Serial.print((buttons & 0x01) ? 1 : 0);
    Serial.print(" R=");         Serial.print((buttons & 0x02) ? 1 : 0);
    Serial.print(" M=");         Serial.println((buttons & 0x04) ? 1 : 0);
    Serial.print("  Movement: X="); Serial.print((int)x);
    Serial.print(" Y=");           Serial.println((int)y);

    if (length >= 4) {
      int8_t wheel = (int8_t)data[3];
      Serial.print("  Extra byte 3 (wheel/vendor): ");
      Serial.println((int)wheel);
    }
    if (length > 4)
      Serial.println("  Additional bytes are device-specific; inspect RAW.");
  } else {
    Serial.println("  WARNING: shorter than the 3-byte Boot Mouse report.");
  }
}

void poseidonMouseInterruptTest() {
  Serial.println();
  Serial.println("========================================");
  Serial.println(" HID BOOT MOUSE - INTERRUPT IN TEST");
  Serial.println("========================================");

  if (!enumAddress1Ready) {
    Serial.println("ERROR: Run POSEIDON, ENUM18 and IDENTIFY first.");
    return;
  }

  uint16_t vid = (uint16_t)enumStoredDeviceDescriptor[8] |
                 ((uint16_t)enumStoredDeviceDescriptor[9] << 8);
  uint16_t pid = (uint16_t)enumStoredDeviceDescriptor[10] |
                 ((uint16_t)enumStoredDeviceDescriptor[11] << 8);
  Serial.print("Device VID:PID = 0x");
  printHexByte((uint8_t)(vid >> 8)); printHexByte((uint8_t)vid);
  Serial.print(":0x");
  printHexByte((uint8_t)(pid >> 8)); printHexByte((uint8_t)pid);
  Serial.println();

  if (vid != 0x145F || pid != 0x02CF) {
    Serial.println("WARNING: current device is not the identified Trust/YICHIP receiver.");
    Serial.println("The test still uses interface 1 / endpoint 0x82 from its known descriptor.");
  }

  Serial.println("Target: interface 1, HID Boot Mouse, endpoint 0x82 IN.");
  Serial.println("Max packet: 64 bytes; bInterval: 2 ms.");

  // SET_CONFIGURATION(1): standard device OUT request.
  if (!enumControlNoData(0x00, 0x09, 1, 0,
                         "SET_CONFIGURATION(1)")) {
    Serial.println("STOP: could not configure USB device.");
    return;
  }

  // SET_PROTOCOL(Boot=0) for HID mouse interface 1.
  if (!enumControlNoData(0x21, 0x0B, 0, HID_MOUSE_INTERFACE,
                         "SET_PROTOCOL(Boot), interface 1")) {
    Serial.println("STOP: mouse interface did not accept Boot Protocol.");
    return;
  }

  // Optional SET_IDLE (duration 0 = indefinitely until changed). Some
  // receivers ignore it; it is not required for the interrupt test, so omit.
  delay(10);

  Serial.println();
  Serial.println("Move the mouse and click its buttons now.");
  Serial.println("Polling for 12 seconds; reports will print after capture.");

  uint8_t captured[HID_MOUSE_MAX_CAPTURED][HID_MOUSE_MAX_PACKET];
  uint8_t capturedLength[HID_MOUSE_MAX_CAPTURED];
  bool capturedData1[HID_MOUSE_MAX_CAPTURED];
  uint8_t capturedCount = 0;
  uint32_t ackCount = 0;
  uint32_t nakCount = 0;
  uint32_t errorCount = 0;
  uint32_t pollCount = 0;
  bool data1 = false; // Interrupt endpoint data toggle starts at DATA0.
  uint32_t startMs = millis();
  uint32_t lastPollUs = 0;
  bool stoppedOnError = false;
  uint8_t lastErrorStatus = 0;

  while ((uint32_t)(millis() - startMs) < HID_MOUSE_TEST_DURATION_MS) {
    // Respect the endpoint's 2 ms polling interval. If a transaction itself
    // takes longer than 2 ms, begin the next poll immediately afterwards.
    if (lastPollUs != 0) {
      while ((uint32_t)(micros() - lastPollUs) <
             (uint32_t)HID_MOUSE_INTERVAL_MS * 1000UL) {
        delayMicroseconds(50);
      }
    }
    lastPollUs = micros();
    pollCount++;

    // One transaction per poll. NAK is normal when no new mouse report exists.
    EnumTransferResult result = enumIssueChannelAEndpoint(
      ENUM_PID_IN, HID_MOUSE_ENDPOINT, false, data1,
      ENUM_NEW_DEVICE_ADDRESS, ENUM_BUFFER_A,
      HID_MOUSE_MAX_PACKET, NULL);

    if (!result.started || !result.done) {
      errorCount++;
      lastErrorStatus = result.hostStatus;
      stoppedOnError = true;
      break;
    }

    if (result.hostStatus & ENUM_HOST_NAK) {
      nakCount++;
      continue; // NAK does not advance the endpoint data toggle.
    }

    if (!result.ack) {
      errorCount++;
      lastErrorStatus = result.hostStatus;
      stoppedOnError = true;
      break;
    }

    if (!result.sequenceOK) {
      errorCount++;
      lastErrorStatus = result.hostStatus;
      stoppedOnError = true;
      break;
    }

    if (result.txLeft > HID_MOUSE_MAX_PACKET) {
      errorCount++;
      lastErrorStatus = result.hostStatus;
      stoppedOnError = true;
      break;
    }

    ackCount++;
    uint8_t received = HID_MOUSE_MAX_PACKET - result.txLeft;

    if (capturedCount < HID_MOUSE_MAX_CAPTURED) {
      capturedLength[capturedCount] = received;
      capturedData1[capturedCount] = data1;
      for (uint8_t i = 0; i < received; i++)
        captured[capturedCount][i] = slReadRAM(ENUM_BUFFER_A + i);
      capturedCount++;
    }

    // Toggle only after a successfully ACKed IN data packet.
    data1 = !data1;
  }

  Serial.println();
  Serial.println("========================================");
  Serial.println(" INTERRUPT IN POLL SUMMARY");
  Serial.println("========================================");
  Serial.print("Poll interval target: ");
  Serial.print(HID_MOUSE_INTERVAL_MS);
  Serial.println(" ms");
  Serial.print("Polls issued: "); Serial.println(pollCount);
  Serial.print("ACK reports: "); Serial.println(ackCount);
  Serial.print("NAK (no new report): "); Serial.println(nakCount);
  Serial.print("Captured reports: "); Serial.println(capturedCount);
  Serial.print("Errors: "); Serial.println(errorCount);
  if (stoppedOnError) {
    Serial.print("Last HOSTSTATUS: 0x");
    printHexByte(lastErrorStatus);
    Serial.println();
  }
  if (ackCount > capturedCount)
    Serial.println("NOTE: capture buffer full; additional ACK reports were counted but not stored.");

  if (capturedCount == 0) {
    Serial.println();
    Serial.println("No interrupt data reports were received.");
    Serial.println("Check that the mouse was moved/clicked and that SET_PROTOCOL succeeded.");
  } else {
    Serial.println();
    Serial.println("CAPTURED MOUSE REPORTS");
    Serial.println("----------------------");
    for (uint8_t i = 0; i < capturedCount; i++) {
      enumPrintBootMouseReport((uint8_t)(i + 1), captured[i],
                               capturedLength[i], capturedData1[i]);
    }
  }

  Serial.println();
  if (errorCount == 0 && ackCount > 0) {
    Serial.println("HID INTERRUPT IN CHECK PASSED: mouse reports received.");
  } else if (errorCount == 0) {
    Serial.println("HID polling ran without controller errors, but no reports arrived.");
  } else {
    Serial.println("HID INTERRUPT IN CHECK STOPPED ON AN UNEXPECTED ERROR.");
  }
  Serial.println("Device remains configured at address 1 until USB reset.");
  Serial.println("Run POSEIDON again before a fresh ENUM18 test.");
  Serial.println("========================================");
}

// ==========================================
// HID MOUSE DIAGNOSTICS: GET_PROTOCOL / GET_REPORT / SET_IDLE
// ==========================================
#define HID_MOUSE_DIAG_POLL_MS       4000UL
#define HID_MOUSE_DIAG_CAPTURED      48
#define HID_MOUSE_IDLE_DURATION      5   // HID units of 4 ms => 20 ms

void poseidonMouseInterruptDiagnostics() {
  Serial.println();
  Serial.println("========================================");
  Serial.println(" HID MOUSE - THREE CONTROL TESTS + INTERRUPT IN");
  Serial.println("========================================");

  if (!enumAddress1Ready) {
    Serial.println("ERROR: Run POSEIDON, ENUM18 and IDENTIFY first.");
    return;
  }

  uint16_t vid = (uint16_t)enumStoredDeviceDescriptor[8] |
                 ((uint16_t)enumStoredDeviceDescriptor[9] << 8);
  uint16_t pid = (uint16_t)enumStoredDeviceDescriptor[10] |
                 ((uint16_t)enumStoredDeviceDescriptor[11] << 8);
  Serial.print("Device VID:PID = 0x");
  printHexByte((uint8_t)(vid >> 8)); printHexByte((uint8_t)vid);
  Serial.print(":0x");
  printHexByte((uint8_t)(pid >> 8)); printHexByte((uint8_t)pid);
  Serial.println();

  if (vid != 0x145F || pid != 0x02CF) {
    Serial.println("WARNING: not the known Trust/YICHIP receiver; using its known interface/endpoint map anyway.");
  }

  Serial.println("Target: HID interface 1, Boot Mouse, endpoint 0x82 IN.");
  Serial.println("EP0 packet size:"); Serial.println(enumStoredEp0PacketSize);

  // Ensure the device is configured and the mouse interface is in Boot mode.
  bool configured = enumControlNoData(0x00, 0x09, 1, 0,
                                      "SET_CONFIGURATION(1)");
  bool bootSet = false;
  if (configured) {
    bootSet = enumControlNoData(0x21, 0x0B, 0, HID_MOUSE_INTERFACE,
                                "SET_PROTOCOL(Boot=0), interface 1");
  }
  if (!configured || !bootSet) {
    Serial.println("STOP: configuration or SET_PROTOCOL failed; diagnostics cannot continue safely.");
    return;
  }

  // TEST 1: GET_PROTOCOL (Boot=0, Report=1).
  Serial.println();
  Serial.println("[TEST 1/3] GET_PROTOCOL");
  uint8_t protocolData[8] = {0};
  uint16_t protocolLength = 0;
  bool getProtocolOK = enumControlIn(
    0xA1, 0x03, 0x0000, HID_MOUSE_INTERFACE,
    1, protocolData, sizeof(protocolData), protocolLength,
    "GET_PROTOCOL, mouse interface 1");

  if (getProtocolOK && protocolLength == 1) {
    Serial.print("GET_PROTOCOL value: ");
    Serial.println(protocolData[0]);
    if (protocolData[0] == 0)
      Serial.println("CONFIRMED: device reports Boot Protocol.");
    else if (protocolData[0] == 1)
      Serial.println("WARNING: device reports Report Protocol, not Boot Protocol.");
    else
      Serial.println("WARNING: unexpected GET_PROTOCOL value.");
  } else {
    Serial.println("GET_PROTOCOL did not return exactly one data byte.");
  }

  // TEST 2: GET_REPORT(Input, Report ID 0), expected Boot Mouse report = 3 bytes.
  Serial.println();
  Serial.println("[TEST 2/3] GET_REPORT(Input, report ID 0)");
  uint8_t reportData[8] = {0};
  uint16_t reportLength = 0;
  bool getReportOK = enumControlIn(
    0xA1, 0x01, 0x0100, HID_MOUSE_INTERFACE,
    3, reportData, sizeof(reportData), reportLength,
    "GET_REPORT(Input), report ID 0, interface 1");

  if (getReportOK) {
    Serial.println("GET_REPORT returned:");
    enumPrintBootMouseReport(0, reportData, (uint8_t)reportLength, true);
    if (reportLength != 3)
      Serial.println("NOTE: Boot Mouse normally returns 3 bytes; inspect the actual length/report format.");
  } else {
    Serial.println("GET_REPORT(Input) failed; continuing to SET_IDLE and Interrupt IN test.");
  }

  // TEST 3: ask the mouse to resend an unchanged Input report every 20 ms.
  Serial.println();
  Serial.println("[TEST 3/3] SET_IDLE");
  Serial.println("Duration=5 HID units x 4 ms = 20 ms; Report ID=0 (all reports).");
  bool setIdleOK = enumControlNoData(
    0x21, 0x0A, (uint16_t)(HID_MOUSE_IDLE_DURATION << 8),
    HID_MOUSE_INTERFACE, "SET_IDLE(20 ms), report ID 0, interface 1");
  if (setIdleOK)
    Serial.println("SET_IDLE accepted. Unchanged reports should be repeated after the idle interval.");
  else
    Serial.println("SET_IDLE was not accepted; continue polling to check movement-triggered reports.");

  // Allow the newly requested idle period to begin before interrupt polling.
  delay(25);
  Serial.println();
  Serial.println("Move the mouse and click its buttons now.");
  Serial.println("Polling endpoint 0x82 at 2 ms intervals for 4 seconds.");
  Serial.println("NAK is normal; DATA toggle advances only after ACK.");

  uint8_t captured[HID_MOUSE_DIAG_CAPTURED][HID_MOUSE_MAX_PACKET];
  uint8_t capturedLength[HID_MOUSE_DIAG_CAPTURED];
  bool capturedData1[HID_MOUSE_DIAG_CAPTURED];
  uint8_t capturedCount = 0;
  uint32_t ackCount = 0;
  uint32_t nakCount = 0;
  uint32_t errorCount = 0;
  uint32_t pollCount = 0;
  bool data1 = false; // Interrupt endpoint starts at DATA0 after configuration.
  uint32_t startMs = millis();
  uint32_t lastPollUs = 0;
  bool stoppedOnError = false;
  uint8_t lastErrorStatus = 0;

  while ((uint32_t)(millis() - startMs) < HID_MOUSE_DIAG_POLL_MS) {
    if (lastPollUs != 0) {
      while ((uint32_t)(micros() - lastPollUs) <
             (uint32_t)HID_MOUSE_INTERVAL_MS * 1000UL) {
        delayMicroseconds(50);
      }
    }
    lastPollUs = micros();
    pollCount++;

    EnumTransferResult result = enumIssueChannelAEndpoint(
      ENUM_PID_IN, HID_MOUSE_ENDPOINT, false, data1,
      ENUM_NEW_DEVICE_ADDRESS, ENUM_BUFFER_A,
      HID_MOUSE_MAX_PACKET, NULL);

    if (!result.started || !result.done) {
      errorCount++;
      lastErrorStatus = result.hostStatus;
      stoppedOnError = true;
      break;
    }

    if (result.hostStatus & ENUM_HOST_NAK) {
      nakCount++;
      continue;
    }

    if (!result.ack || !result.sequenceOK ||
        result.txLeft > HID_MOUSE_MAX_PACKET) {
      errorCount++;
      lastErrorStatus = result.hostStatus;
      stoppedOnError = true;
      break;
    }

    ackCount++;
    uint8_t received = HID_MOUSE_MAX_PACKET - result.txLeft;
    if (capturedCount < HID_MOUSE_DIAG_CAPTURED) {
      capturedLength[capturedCount] = received;
      capturedData1[capturedCount] = data1;
      for (uint8_t i = 0; i < received; i++)
        captured[capturedCount][i] = slReadRAM(ENUM_BUFFER_A + i);
      capturedCount++;
    }
    data1 = !data1;
  }

  Serial.println();
  Serial.println("========================================");
  Serial.println(" MOUSEIRQ2 INTERRUPT POLL SUMMARY");
  Serial.println("========================================");
  Serial.print("Poll interval target: "); Serial.print(HID_MOUSE_INTERVAL_MS); Serial.println(" ms");
  Serial.print("Polls issued: "); Serial.println(pollCount);
  Serial.print("ACK reports: "); Serial.println(ackCount);
  Serial.print("NAK (no report at this poll): "); Serial.println(nakCount);
  Serial.print("Captured reports: "); Serial.println(capturedCount);
  Serial.print("Errors: "); Serial.println(errorCount);
  if (stoppedOnError) {
    Serial.print("Last HOSTSTATUS: 0x"); printHexByte(lastErrorStatus); Serial.println();
  }

  if (capturedCount > 0) {
    Serial.println();
    Serial.println("CAPTURED INTERRUPT REPORTS");
    Serial.println("--------------------------");
    for (uint8_t i = 0; i < capturedCount; i++)
      enumPrintBootMouseReport((uint8_t)(i + 1), captured[i],
                               capturedLength[i], capturedData1[i]);
  } else {
    Serial.println("No Interrupt IN reports captured.");
  }

  Serial.println();
  Serial.print("GET_PROTOCOL: "); Serial.println(getProtocolOK ? "CONTROL TRANSFER OK" : "FAILED");
  Serial.print("GET_REPORT:   "); Serial.println(getReportOK ? "CONTROL TRANSFER OK" : "FAILED");
  Serial.print("SET_IDLE:     "); Serial.println(setIdleOK ? "ACK" : "FAILED / STALL");
  if (errorCount == 0 && ackCount > 0)
    Serial.println("INTERRUPT IN: REPORTS RECEIVED.");
  else if (errorCount == 0)
    Serial.println("INTERRUPT IN: polling ran, but no reports were received.");
  else
    Serial.println("INTERRUPT IN: stopped on a controller/transfer error.");

  Serial.println("Device remains configured at address 1 until USB reset.");
  Serial.println("Run POSEIDON before a fresh ENUM18 sequence.");
  Serial.println("========================================");
}


// ==========================================
// HID REPORT DESCRIPTOR + FULL FEATURE PROBE
// ==========================================
#define HID_MOUSE_REPORT_DESC_LENGTH 163
#define HID_MOUSE_REPORT_DESC_CAPACITY 192
#define HID_MOUSE_FEATURE_POLL_MS 4000UL
#define HID_MOUSE_FEATURE_CAPTURED 24

struct HidGlobalState {
  uint32_t usagePage;
  int32_t logicalMin;
  int32_t logicalMax;
  uint32_t physicalMin;
  uint32_t physicalMax;
  uint32_t unit;
  int8_t unitExponent;
  uint8_t reportSize;
  uint8_t reportId;
  uint16_t reportCount;
};

uint32_t hidReadUnsigned(const uint8_t *data, uint8_t size) {
  uint32_t value = 0;
  for (uint8_t i = 0; i < size; i++) value |= ((uint32_t)data[i] << (8U * i));
  return value;
}

int32_t hidReadSigned(const uint8_t *data, uint8_t size) {
  uint32_t raw = hidReadUnsigned(data, size);
  if (size == 0) return 0;
  uint8_t bits = (uint8_t)(size * 8U);
  if (bits < 32 && (raw & ((uint32_t)1 << (bits - 1U))))
    raw |= (~(uint32_t)0) << bits;
  return (int32_t)raw;
}

void hidPrintUsage(uint32_t page, uint32_t usage) {
  if (page == 0x01) {
    switch (usage) {
      case 0x01: Serial.print("Pointer"); return;
      case 0x02: Serial.print("Mouse"); return;
      case 0x30: Serial.print("X"); return;
      case 0x31: Serial.print("Y"); return;
      case 0x32: Serial.print("Z"); return;
      case 0x33: Serial.print("Rx"); return;
      case 0x34: Serial.print("Ry"); return;
      case 0x35: Serial.print("Rz"); return;
      case 0x36: Serial.print("Slider"); return;
      case 0x37: Serial.print("Dial"); return;
      case 0x38: Serial.print("Wheel"); return;
      case 0x39: Serial.print("Hat switch"); return;
      default: break;
    }
  } else if (page == 0x09) {
    Serial.print("Button "); Serial.print(usage); return;
  } else if (page == 0x0C) {
    switch (usage) {
      case 0x0238: Serial.print("AC Pan"); return;
      case 0x00E9: Serial.print("Volume Up"); return;
      case 0x00EA: Serial.print("Volume Down"); return;
      case 0x00CD: Serial.print("Play/Pause"); return;
      case 0x00B5: Serial.print("Scan Next Track"); return;
      case 0x00B6: Serial.print("Scan Previous Track"); return;
      default: break;
    }
  }
  Serial.print("UsagePage 0x"); Serial.print((unsigned long)page, HEX);
  Serial.print(" / Usage 0x"); Serial.print((unsigned long)usage, HEX);
}

void hidPrintUsageSet(const HidGlobalState &g,
                      const uint32_t *usages, uint8_t usageCount,
                      bool hasMin, uint32_t usageMin,
                      bool hasMax, uint32_t usageMax,
                      uint8_t reportCount) {
  Serial.print("    Usages: ");
  if (usageCount) {
    uint8_t shown = usageCount < reportCount ? usageCount : reportCount;
    for (uint8_t i = 0; i < shown; i++) {
      if (i) Serial.print(", ");
      hidPrintUsage(g.usagePage, usages[i]);
    }
    if (usageCount < reportCount && hasMin && hasMax) {
      Serial.print("; range ");
      hidPrintUsage(g.usagePage, usageMin);
      Serial.print(" .. ");
      hidPrintUsage(g.usagePage, usageMax);
    }
  } else if (hasMin || hasMax) {
    if (hasMin) hidPrintUsage(g.usagePage, usageMin);
    Serial.print(" .. ");
    if (hasMax) hidPrintUsage(g.usagePage, usageMax);
  } else {
    Serial.print("(not explicitly assigned)");
  }
  Serial.println();
}

void hidPrintMainFlags(uint32_t flags) {
  Serial.print((flags & 0x01) ? "Constant" : "Data");
  Serial.print(", ");
  Serial.print((flags & 0x02) ? "Variable" : "Array");
  Serial.print(", ");
  Serial.print((flags & 0x04) ? "Relative" : "Absolute");
  if (flags & 0x40) Serial.print(", NullState");
  if (flags & 0x100) Serial.print(", BufferedBytes");
}

void hidParseReportDescriptor(const uint8_t *desc, uint16_t length) {
  HidGlobalState g = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  HidGlobalState globalStack[6];
  uint8_t globalDepth = 0;
  uint32_t localUsages[24];
  uint8_t localUsageCount = 0;
  bool hasUsageMin = false, hasUsageMax = false;
  uint32_t usageMin = 0, usageMax = 0;
  uint16_t inputBits[256];
  uint16_t outputBits[256];
  uint16_t featureBits[256];
  memset(inputBits, 0, sizeof(inputBits));
  memset(outputBits, 0, sizeof(outputBits));
  memset(featureBits, 0, sizeof(featureBits));

  Serial.println();
  Serial.println("HID REPORT DESCRIPTOR DECODE");
  Serial.println("----------------------------");

  uint16_t pos = 0;
  uint16_t itemNumber = 0;
  while (pos < length) {
    uint8_t prefix = desc[pos++];
    itemNumber++;
    if (prefix == 0xFE) {
      if (pos + 2 > length) { Serial.println("Malformed long item header."); break; }
      uint8_t longLength = desc[pos++];
      uint8_t longTag = desc[pos++];
      Serial.print("  Long item tag=0x"); printHexByte(longTag);
      Serial.print(" length="); Serial.println(longLength);
      if ((uint32_t)pos + longLength > length) { Serial.println("Malformed long item payload."); break; }
      pos += longLength;
      continue;
    }

    uint8_t sizeCode = prefix & 0x03;
    uint8_t itemSize = (sizeCode == 3) ? 4 : sizeCode;
    uint8_t itemType = (prefix >> 2) & 0x03;
    uint8_t itemTag = (prefix >> 4) & 0x0F;
    if ((uint32_t)pos + itemSize > length) { Serial.println("Malformed short item payload."); break; }
    uint32_t uvalue = hidReadUnsigned(desc + pos, itemSize);
    int32_t svalue = hidReadSigned(desc + pos, itemSize);
    pos += itemSize;

    if (itemType == 1) { // Global
      switch (itemTag) {
        case 0: g.usagePage = uvalue; break;
        case 1: g.logicalMin = svalue; break;
        case 2: g.logicalMax = (g.logicalMin < 0) ? svalue : (int32_t)uvalue; break;
        case 3: g.physicalMin = uvalue; break;
        case 4: g.physicalMax = uvalue; break;
        case 5: g.unitExponent = (int8_t)(svalue & 0x0F); break;
        case 6: g.unit = uvalue; break;
        case 7: g.reportSize = (uint8_t)uvalue; break;
        case 8: g.reportId = (uint8_t)uvalue; break;
        case 9: g.reportCount = (uint16_t)uvalue; break;
        case 10:
          if (globalDepth < 6) globalStack[globalDepth++] = g;
          else Serial.println("WARNING: HID global PUSH stack overflow.");
          break;
        case 11:
          if (globalDepth) g = globalStack[--globalDepth];
          else Serial.println("WARNING: HID global POP underflow.");
          break;
        default: break;
      }
    } else if (itemType == 2) { // Local
      switch (itemTag) {
        case 0:
          if (localUsageCount < 24) {
            uint32_t usage = uvalue;
            if (itemSize == 4 && (uvalue >> 16)) usage = uvalue & 0xFFFF;
            localUsages[localUsageCount++] = usage;
          }
          break;
        case 1:
          hasUsageMin = true;
          usageMin = (itemSize == 4 && (uvalue >> 16)) ? (uvalue & 0xFFFF) : uvalue;
          break;
        case 2:
          hasUsageMax = true;
          usageMax = (itemSize == 4 && (uvalue >> 16)) ? (uvalue & 0xFFFF) : uvalue;
          break;
        default: break;
      }
    } else if (itemType == 0) { // Main
      if (itemTag == 8 || itemTag == 9 || itemTag == 11) {
        uint16_t bits = (uint16_t)(g.reportSize * g.reportCount);
        uint16_t *offsets = (itemTag == 8) ? inputBits :
                            (itemTag == 9) ? outputBits : featureBits;
        uint16_t offset = offsets[g.reportId];
        Serial.print("  ");
        Serial.print(itemTag == 8 ? "INPUT" : itemTag == 9 ? "OUTPUT" : "FEATURE");
        Serial.print(" reportID="); Serial.print(g.reportId);
        Serial.print(" bitOffset="); Serial.print(offset);
        Serial.print(" size="); Serial.print(g.reportSize);
        Serial.print(" count="); Serial.print(g.reportCount);
        Serial.print(" bits="); Serial.println(bits);
        Serial.print("    Flags: "); hidPrintMainFlags(uvalue); Serial.println();
        hidPrintUsageSet(g, localUsages, localUsageCount,
                         hasUsageMin, usageMin, hasUsageMax, usageMax,
                         (g.reportCount > 255) ? 255 : (uint8_t)g.reportCount);
        if (itemTag == 8 && !(uvalue & 0x01) && (uvalue & 0x02)) {
          Serial.print("    Logical range: "); Serial.print(g.logicalMin);
          Serial.print(" .. "); Serial.println(g.logicalMax);
        }
        offsets[g.reportId] = (uint16_t)(offset + bits);
      } else if (itemTag == 10) {
        Serial.print("  COLLECTION type="); Serial.print(uvalue);
        Serial.print(" usage=");
        if (localUsageCount) hidPrintUsage(g.usagePage, localUsages[0]);
        else Serial.print("(none)");
        Serial.println();
      } else if (itemTag == 12) {
        Serial.println("  END COLLECTION");
      }
      // Local items are scoped to the next Main item only.
      localUsageCount = 0;
      hasUsageMin = false;
      hasUsageMax = false;
      usageMin = usageMax = 0;
    }
  }

  Serial.println();
  Serial.println("HID REPORT LENGTH SUMMARY");
  bool any = false;
  for (uint16_t id = 0; id < 256; id++) {
    if (inputBits[id]) {
      any = true;
      uint16_t bytes = (uint16_t)((inputBits[id] + 7U) / 8U);
      Serial.print("  Input report ID "); Serial.print(id);
      Serial.print(": "); Serial.print(inputBits[id]);
      Serial.print(" bits, "); Serial.print(bytes);
      Serial.print(" payload bytes; on-wire length ");
      Serial.println(bytes + (id ? 1 : 0));
    }
  }
  for (uint16_t id = 0; id < 256; id++) {
    if (outputBits[id]) {
      any = true;
      Serial.print("  Output report ID "); Serial.print(id);
      Serial.print(": "); Serial.print((outputBits[id] + 7U) / 8U);
      Serial.println(" payload bytes");
    }
    if (featureBits[id]) {
      any = true;
      Serial.print("  Feature report ID "); Serial.print(id);
      Serial.print(": "); Serial.print((featureBits[id] + 7U) / 8U);
      Serial.println(" payload bytes");
    }
  }
  if (!any) Serial.println("  No Input/Output/Feature main items were decoded.");
}

void enumPrintRawHidReport(const uint8_t *data, uint8_t length,
                           uint8_t reportNumber, bool data1) {
  Serial.print("Report #"); Serial.print(reportNumber);
  Serial.print(" DATA"); Serial.print(data1 ? 1 : 0);
  Serial.print(" LEN="); Serial.println(length);
  Serial.print("  RAW:");
  for (uint8_t i = 0; i < length; i++) {
    Serial.print(' '); printHexByte(data[i]);
  }
  Serial.println();
}

void poseidonMouseFeatures() {
  Serial.println();
  Serial.println("========================================");
  Serial.println(" HID MOUSE - REPORT DESCRIPTOR + REPORT PROTOCOL");
  Serial.println("========================================");

  if (!enumAddress1Ready) {
    Serial.println("ERROR: Run POSEIDON, ENUM18 and IDENTIFY first.");
    return;
  }

  uint16_t vid = (uint16_t)enumStoredDeviceDescriptor[8] |
                 ((uint16_t)enumStoredDeviceDescriptor[9] << 8);
  uint16_t pid = (uint16_t)enumStoredDeviceDescriptor[10] |
                 ((uint16_t)enumStoredDeviceDescriptor[11] << 8);
  Serial.print("Device VID:PID = 0x");
  printHexByte((uint8_t)(vid >> 8)); printHexByte((uint8_t)vid);
  Serial.print(":0x");
  printHexByte((uint8_t)(pid >> 8)); printHexByte((uint8_t)pid);
  Serial.println();
  Serial.println("HID mouse interface=1, endpoint=0x82 IN, bInterval=2 ms.");

  if (!enumControlNoData(0x00, 0x09, 1, 0, "SET_CONFIGURATION(1)")) {
    Serial.println("STOP: SET_CONFIGURATION failed.");
    return;
  }

  uint8_t reportDescriptor[HID_MOUSE_REPORT_DESC_CAPACITY] = {0};
  uint16_t reportDescriptorLength = 0;
  Serial.println();
  Serial.println("[1] GET_DESCRIPTOR(Report), interface 1");
  bool descriptorOK = enumControlIn(
    0x81, 0x06, 0x2200, HID_MOUSE_INTERFACE,
    HID_MOUSE_REPORT_DESC_LENGTH, reportDescriptor,
    sizeof(reportDescriptor), reportDescriptorLength,
    "HID Report Descriptor, type 0x22, interface 1");

  if (!descriptorOK || reportDescriptorLength == 0) {
    Serial.println("STOP: Could not read HID Report Descriptor.");
    return;
  }

  Serial.println();
  Serial.print("Report descriptor received: ");
  Serial.print(reportDescriptorLength); Serial.println(" bytes.");
  Serial.println("RAW HID REPORT DESCRIPTOR:");
  for (uint16_t i = 0; i < reportDescriptorLength; i++) {
    if ((i % 16) == 0) Serial.println();
    printHexByte(reportDescriptor[i]); Serial.print(' ');
  }
  Serial.println();
  hidParseReportDescriptor(reportDescriptor, reportDescriptorLength);

  Serial.println();
  Serial.println("[2] SET_PROTOCOL(Report=1), interface 1");
  bool reportProtocolSet = enumControlNoData(
    0x21, 0x0B, 1, HID_MOUSE_INTERFACE,
    "SET_PROTOCOL(Report=1), mouse interface 1");

  uint8_t protocol = 0xFF;
  uint16_t protocolLength = 0;
  bool protocolRead = enumControlIn(
    0xA1, 0x03, 0, HID_MOUSE_INTERFACE, 1,
    &protocol, 1, protocolLength,
    "GET_PROTOCOL after SET_PROTOCOL(Report)");
  Serial.print("GET_PROTOCOL after switch: ");
  if (protocolRead && protocolLength == 1) {
    Serial.println(protocol);
    if (protocol == 1) Serial.println("CONFIRMED: Report Protocol active.");
    else Serial.println("WARNING: device did not report Report Protocol.");
  } else {
    Serial.println("read failed");
  }
  if (!reportProtocolSet || !protocolRead || protocolLength != 1 || protocol != 1) {
    Serial.println("STOP: Report Protocol could not be confirmed; no interrupt polling performed.");
    return;
  }

  bool setIdleOK = enumControlNoData(
    0x21, 0x0A, (uint16_t)(5U << 8), HID_MOUSE_INTERFACE,
    "SET_IDLE(20 ms), all report IDs");
  Serial.print("SET_IDLE: "); Serial.println(setIdleOK ? "ACK" : "failed / stalled");
  delay(25);

  Serial.println();
  Serial.println("[3] REPORT PROTOCOL INTERRUPT IN POLL");
  Serial.println("Move the mouse, click buttons, and use the wheel now.");
  Serial.println("Polling endpoint 0x82 for 4 seconds; NAK is normal.");

  uint8_t captured[HID_MOUSE_FEATURE_CAPTURED][HID_MOUSE_MAX_PACKET];
  uint8_t capturedLength[HID_MOUSE_FEATURE_CAPTURED];
  bool capturedData1[HID_MOUSE_FEATURE_CAPTURED];
  uint8_t capturedCount = 0;
  uint32_t pollCount = 0, ackCount = 0, nakCount = 0, errorCount = 0;
  bool data1 = false;
  uint32_t startMs = millis();
  uint32_t lastPollUs = 0;
  uint8_t lastErrorStatus = 0;
  bool stoppedOnError = false;

  while ((uint32_t)(millis() - startMs) < HID_MOUSE_FEATURE_POLL_MS) {
    if (lastPollUs != 0) {
      while ((uint32_t)(micros() - lastPollUs) <
             (uint32_t)HID_MOUSE_INTERVAL_MS * 1000UL) delayMicroseconds(50);
    }
    lastPollUs = micros();
    pollCount++;
    EnumTransferResult result = enumIssueChannelAEndpoint(
      ENUM_PID_IN, HID_MOUSE_ENDPOINT, false, data1,
      ENUM_NEW_DEVICE_ADDRESS, ENUM_BUFFER_A,
      HID_MOUSE_MAX_PACKET, NULL);

    if (!result.started || !result.done) {
      errorCount++; lastErrorStatus = result.hostStatus;
      stoppedOnError = true; break;
    }
    if (result.hostStatus & ENUM_HOST_NAK) { nakCount++; continue; }
    if (!result.ack || !result.sequenceOK || result.txLeft > HID_MOUSE_MAX_PACKET) {
      errorCount++; lastErrorStatus = result.hostStatus;
      stoppedOnError = true; break;
    }

    ackCount++;
    uint8_t received = HID_MOUSE_MAX_PACKET - result.txLeft;
    if (capturedCount < HID_MOUSE_FEATURE_CAPTURED) {
      capturedLength[capturedCount] = received;
      capturedData1[capturedCount] = data1;
      for (uint8_t i = 0; i < received; i++)
        captured[capturedCount][i] = slReadRAM(ENUM_BUFFER_A + i);
      capturedCount++;
    }
    data1 = !data1;
  }

  Serial.println();
  Serial.println("========================================");
  Serial.println(" REPORT PROTOCOL POLL SUMMARY");
  Serial.println("========================================");
  Serial.print("Polls: "); Serial.println(pollCount);
  Serial.print("ACK reports: "); Serial.println(ackCount);
  Serial.print("NAK: "); Serial.println(nakCount);
  Serial.print("Captured: "); Serial.println(capturedCount);
  Serial.print("Errors: "); Serial.println(errorCount);
  if (stoppedOnError) {
    Serial.print("Last HOSTSTATUS=0x"); printHexByte(lastErrorStatus); Serial.println();
  }
  if (capturedCount) {
    Serial.println(); Serial.println("RAW REPORT PROTOCOL INPUT REPORTS");
    for (uint8_t i = 0; i < capturedCount; i++)
      enumPrintRawHidReport(captured[i], capturedLength[i],
                            (uint8_t)(i + 1), capturedData1[i]);
  }
  Serial.println();
  Serial.println("No SET_CONFIGURATION reset or device reset was issued after capture.");
  Serial.println("Use the descriptor map above to interpret report IDs, buttons, axes and wheel fields.");
  Serial.println("Run POSEIDON before a fresh ENUM18 sequence.");
  Serial.println("========================================");
}




// ==========================================
// FULL HID INPUT FUNCTION TEST
// ==========================================
// Trust/YICHIP receiver descriptor confirmed on hardware:
// Report ID 1: 5 buttons, signed 16-bit X/Y, Wheel, AC Pan (8 bytes on wire)
// Report ID 2: System Control bitmap (2 bytes on wire)
// Report ID 3: Consumer Control usage (3 bytes on wire)
// Report ID 0xBA: vendor-specific IN report (32 bytes on wire)
// Report ID 0xBA also has a vendor-specific OUT report; this test NEVER writes it.

#define MOUSE_INPUTS_INTERFACE       1
#define MOUSE_INPUTS_ENDPOINT        2
#define MOUSE_INPUTS_ENDPOINT_ADDR   0x82
#define MOUSE_INPUTS_MAX_PACKET      64
#define MOUSE_INPUTS_INTERVAL_MS     2
#define MOUSE_INPUTS_STAGE_MS        4000UL
#define MOUSE_INPUTS_CAPTURE_MAX     80
#define MOUSE_INPUTS_STAGE_COUNT     6

int32_t mouseReadS16LE(const uint8_t *p) {
  uint16_t raw = (uint16_t)p[0] | ((uint16_t)p[1] << 8);
  return (raw & 0x8000U) ? ((int32_t)raw - 65536L) : (int32_t)raw;
}

int16_t mouseReadS8(uint8_t value) {
  return (value & 0x80U) ? ((int16_t)value - 256) : (int16_t)value;
}

void mousePrintConsumerUsage(uint16_t usage) {
  const char *name = NULL;
  switch (usage) {
    case 0x0000: name = "No control"; break;
    case 0x00B0: name = "Play"; break;
    case 0x00B1: name = "Pause"; break;
    case 0x00B2: name = "Record"; break;
    case 0x00B3: name = "Fast Forward"; break;
    case 0x00B4: name = "Rewind"; break;
    case 0x00B5: name = "Scan Next Track"; break;
    case 0x00B6: name = "Scan Previous Track"; break;
    case 0x00B7: name = "Stop"; break;
    case 0x00B8: name = "Eject"; break;
    case 0x00CD: name = "Play/Pause"; break;
    case 0x00E2: name = "Mute"; break;
    case 0x00E9: name = "Volume Increment"; break;
    case 0x00EA: name = "Volume Decrement"; break;
    default: break;
  }
  Serial.print("  Consumer usage=0x");
  printHexByte((uint8_t)(usage >> 8));
  printHexByte((uint8_t)usage);
  if (name) {
    Serial.print(" (");
    Serial.print(name);
    Serial.print(")");
  }
  Serial.println();
}

void mousePrintDetailedReport(const uint8_t *data,
                              uint8_t length,
                              uint8_t reportNumber,
                              uint8_t stage,
                              bool data1) {
  Serial.print("Report #"); Serial.print(reportNumber);
  Serial.print(" [stage "); Serial.print(stage);
  Serial.print("] DATA"); Serial.print(data1 ? 1 : 0);
  Serial.print(" LEN="); Serial.println(length);
  Serial.print("  RAW:");
  for (uint8_t i = 0; i < length; i++) {
    Serial.print(' '); printHexByte(data[i]);
  }
  Serial.println();

  if (length == 0) {
    Serial.println("  WARNING: zero-length IN report.");
    return;
  }

  uint8_t reportId = data[0];
  if (reportId == 0x01) {
    if (length != 8) {
      Serial.print("  Report ID 1 length mismatch: expected 8, got ");
      Serial.println(length);
      return;
    }
    uint8_t buttons = data[1];
    Serial.print("  Mouse buttons:");
    for (uint8_t bit = 0; bit < 5; bit++) {
      Serial.print(" B"); Serial.print(bit + 1);
      Serial.print('='); Serial.print((buttons >> bit) & 1U);
    }
    int32_t x = mouseReadS16LE(&data[2]);
    int32_t y = mouseReadS16LE(&data[4]);
    int16_t wheel = mouseReadS8(data[6]);
    int16_t pan = mouseReadS8(data[7]);
    Serial.print("  X="); Serial.print(x);
    Serial.print("  Y="); Serial.print(y);
    Serial.print("  Wheel="); Serial.print(wheel);
    Serial.print("  AC_Pan="); Serial.println(pan);
    return;
  }

  if (reportId == 0x02) {
    if (length != 2) {
      Serial.print("  Report ID 2 length mismatch: expected 2, got ");
      Serial.println(length);
      return;
    }
    static const char *systemNames[8] = {
      "Power Down", "Sleep", "Wake Up", "Context Menu",
      "Main Menu", "App Menu", "Menu Help", "Menu Exit"
    };
    Serial.print("  System Control bitmap=0x");
    printHexByte(data[1]);
    Serial.println();
    bool active = false;
    for (uint8_t bit = 0; bit < 8; bit++) {
      if (data[1] & (1U << bit)) {
        Serial.print("    ACTIVE: "); Serial.println(systemNames[bit]);
        active = true;
      }
    }
    if (!active) Serial.println("    No System Control usage active.");
    return;
  }

  if (reportId == 0x03) {
    if (length != 3) {
      Serial.print("  Report ID 3 length mismatch: expected 3, got ");
      Serial.println(length);
      return;
    }
    uint16_t usage = (uint16_t)data[1] | ((uint16_t)data[2] << 8);
    Serial.println("  Consumer Control:");
    mousePrintConsumerUsage(usage);
    return;
  }

  if (reportId == 0xBA) {
    if (length != 32) {
      Serial.print("  Vendor report 0xBA length mismatch: expected 32, got ");
      Serial.println(length);
    } else {
      Serial.println("  Vendor-specific input report 0xBA (31 payload bytes).");
      Serial.println("  Raw data only; no vendor OUT command is sent.");
    }
    return;
  }

  Serial.print("  UNKNOWN / UNEXPECTED REPORT ID=0x");
  printHexByte(reportId);
  Serial.println("; kept raw, not interpreted.");
}

bool mouseGetInputReport(uint8_t reportId,
                         uint8_t expectedWireLength,
                         uint8_t *buffer,
                         uint16_t &actualLength) {
  char description[48];
  snprintf(description, sizeof(description), "GET_REPORT(Input), report ID 0x%02X", reportId);
  uint16_t wValue = (uint16_t)(0x0100U | reportId);
  bool ok = enumControlIn(
    0xA1, 0x01, wValue, MOUSE_INPUTS_INTERFACE,
    expectedWireLength, buffer, MOUSE_INPUTS_MAX_PACKET,
    actualLength, description);

  if (!ok) {
    Serial.print("GET_REPORT ID 0x"); printHexByte(reportId);
    Serial.println(" not returned successfully (STALL/transfer failure is recorded, test continues).");
    return false;
  }

  Serial.print("GET_REPORT ID 0x"); printHexByte(reportId);
  Serial.print(" returned "); Serial.print(actualLength);
  Serial.println(" bytes:");
  mousePrintDetailedReport(buffer, (uint8_t)actualLength, 0, 0, true);
  return true;
}

void mousePollInputStage(uint8_t stage,
                         const char *prompt,
                         uint8_t captured[MOUSE_INPUTS_CAPTURE_MAX][MOUSE_INPUTS_MAX_PACKET],
                         uint8_t lengths[MOUSE_INPUTS_CAPTURE_MAX],
                         uint8_t stages[MOUSE_INPUTS_CAPTURE_MAX],
                         bool toggles[MOUSE_INPUTS_CAPTURE_MAX],
                         uint8_t &capturedCount,
                         bool &data1,
                         bool &stoppedOnError,
                         uint32_t &pollCount,
                         uint32_t &ackCount,
                         uint32_t &nakCount,
                         uint32_t &errorCount,
                         uint8_t &lastErrorStatus) {
  if (stoppedOnError) return;
  Serial.println();
  Serial.print("[ACTION "); Serial.print(stage); Serial.print("/6] ");
  Serial.println(prompt);
  Serial.println("Polling now for 4 seconds; perform the action during this window.");

  uint32_t startMs = millis();
  uint32_t lastPollUs = 0;

  while ((uint32_t)(millis() - startMs) < MOUSE_INPUTS_STAGE_MS) {
    if (lastPollUs != 0) {
      while ((uint32_t)(micros() - lastPollUs) <
             (uint32_t)MOUSE_INPUTS_INTERVAL_MS * 1000UL) {
        delayMicroseconds(50);
      }
    }
    lastPollUs = micros();
    pollCount++;

    EnumTransferResult result = enumIssueChannelAEndpoint(
      ENUM_PID_IN, MOUSE_INPUTS_ENDPOINT, false, data1,
      ENUM_NEW_DEVICE_ADDRESS, ENUM_BUFFER_A,
      MOUSE_INPUTS_MAX_PACKET, NULL);

    if (!result.started || !result.done) {
      errorCount++;
      lastErrorStatus = result.hostStatus;
      stoppedOnError = true;
      break;
    }
    if (result.hostStatus & ENUM_HOST_NAK) {
      nakCount++;
      continue; // NAK does not consume the current DATA toggle.
    }
    if (!result.ack || !result.sequenceOK || result.txLeft > MOUSE_INPUTS_MAX_PACKET) {
      errorCount++;
      lastErrorStatus = result.hostStatus;
      stoppedOnError = true;
      break;
    }

    ackCount++;
    uint8_t received = (uint8_t)(MOUSE_INPUTS_MAX_PACKET - result.txLeft);
    if (received == 0) {
      errorCount++;
      lastErrorStatus = result.hostStatus;
      stoppedOnError = true;
      break;
    }

    if (capturedCount < MOUSE_INPUTS_CAPTURE_MAX) {
      lengths[capturedCount] = received;
      stages[capturedCount] = stage;
      toggles[capturedCount] = data1;
      for (uint8_t i = 0; i < received; i++)
        captured[capturedCount][i] = slReadRAM(ENUM_BUFFER_A + i);
      capturedCount++;
    }
    data1 = !data1; // Advance only after a valid ACKed IN packet.
  }
}

void poseidonMouseInputTests() {
  Serial.println();
  Serial.println("========================================");
  Serial.println(" TRUST/YICHIP MOUSE - ALL INPUT REPORTS");
  Serial.println("========================================");

  if (!enumAddress1Ready) {
    Serial.println("ERROR: Run POSEIDON and ENUM18 first.");
    return;
  }

  uint16_t vid = (uint16_t)enumStoredDeviceDescriptor[8] |
                 ((uint16_t)enumStoredDeviceDescriptor[9] << 8);
  uint16_t pid = (uint16_t)enumStoredDeviceDescriptor[10] |
                 ((uint16_t)enumStoredDeviceDescriptor[11] << 8);
  Serial.print("Device VID:PID = 0x");
  printHexByte((uint8_t)(vid >> 8)); printHexByte((uint8_t)vid);
  Serial.print(":0x");
  printHexByte((uint8_t)(pid >> 8)); printHexByte((uint8_t)pid);
  Serial.println();

  if (vid != 0x145F || pid != 0x02CF) {
    Serial.println("STOP: This diagnostic is specifically mapped for Trust/YICHIP VID:PID 145F:02CF.");
    return;
  }

  if (!enumControlNoData(0x00, 0x09, 1, 0, "SET_CONFIGURATION(1)")) {
    Serial.println("STOP: SET_CONFIGURATION failed.");
    return;
  }

  if (!enumControlNoData(0x21, 0x0B, 1, MOUSE_INPUTS_INTERFACE,
                         "SET_PROTOCOL(Report=1), interface 1")) {
    Serial.println("STOP: SET_PROTOCOL(Report) failed.");
    return;
  }

  uint8_t protocol = 0xFF;
  uint16_t protocolLength = 0;
  if (!enumControlIn(0xA1, 0x03, 0, MOUSE_INPUTS_INTERFACE, 1,
                     &protocol, 1, protocolLength,
                     "GET_PROTOCOL after SET_PROTOCOL(Report)" ) ||
      protocolLength != 1 || protocol != 1) {
    Serial.println("STOP: Report Protocol was not confirmed.");
    return;
  }
  Serial.println("Report Protocol confirmed.");

  Serial.println();
  Serial.println("[1] GET_REPORT(Input) by Report ID");
  Serial.println("HID defines these replies in the same format as Interrupt IN reports.");
  uint8_t getReportBuffer[MOUSE_INPUTS_MAX_PACKET] = {0};
  uint16_t getReportLength = 0;
  mouseGetInputReport(0x01, 8, getReportBuffer, getReportLength);
  mouseGetInputReport(0x02, 2, getReportBuffer, getReportLength);
  mouseGetInputReport(0x03, 3, getReportBuffer, getReportLength);
  mouseGetInputReport(0xBA, 32, getReportBuffer, getReportLength);

  Serial.println();
  Serial.println("[2] SET_IDLE(0), all Report IDs");
  Serial.println("This requests event-driven reporting without periodic idle repeats.");
  bool idleOK = enumControlNoData(0x21, 0x0A, 0x0000,
                                  MOUSE_INPUTS_INTERFACE,
                                  "SET_IDLE(0), all report IDs");
  Serial.print("SET_IDLE(0): "); Serial.println(idleOK ? "ACK" : "FAILED / STALL");

  uint8_t captured[MOUSE_INPUTS_CAPTURE_MAX][MOUSE_INPUTS_MAX_PACKET];
  uint8_t lengths[MOUSE_INPUTS_CAPTURE_MAX];
  uint8_t stages[MOUSE_INPUTS_CAPTURE_MAX];
  bool toggles[MOUSE_INPUTS_CAPTURE_MAX];
  uint8_t capturedCount = 0;
  bool data1 = false; // One continuous DATA toggle across all six test stages.
  bool stoppedOnError = false;
  uint32_t pollCount = 0, ackCount = 0, nakCount = 0, errorCount = 0;
  uint8_t lastErrorStatus = 0;

  Serial.println();
  Serial.println("[3] Six staged physical-input tests");
  Serial.println("No serial output is generated during each 4-second polling window.");

  mousePollInputStage(1, "Move X/Y; click buttons 1, 2 and 3.",
                      captured, lengths, stages, toggles, capturedCount, data1, stoppedOnError,
                      pollCount, ackCount, nakCount, errorCount, lastErrorStatus);
  mousePollInputStage(2, "Press side buttons 4 and 5 separately, if present.",
                      captured, lengths, stages, toggles, capturedCount, data1, stoppedOnError,
                      pollCount, ackCount, nakCount, errorCount, lastErrorStatus);
  mousePollInputStage(3, "Scroll the wheel up and down.",
                      captured, lengths, stages, toggles, capturedCount, data1, stoppedOnError,
                      pollCount, ackCount, nakCount, errorCount, lastErrorStatus);
  mousePollInputStage(4, "Tilt the wheel left/right for AC Pan, if supported.",
                      captured, lengths, stages, toggles, capturedCount, data1, stoppedOnError,
                      pollCount, ackCount, nakCount, errorCount, lastErrorStatus);
  mousePollInputStage(5, "Try any system/media buttons or special controls.",
                      captured, lengths, stages, toggles, capturedCount, data1, stoppedOnError,
                      pollCount, ackCount, nakCount, errorCount, lastErrorStatus);
  mousePollInputStage(6, "Try any remaining mouse button or macro control.",
                      captured, lengths, stages, toggles, capturedCount, data1, stoppedOnError,
                      pollCount, ackCount, nakCount, errorCount, lastErrorStatus);

  uint16_t id1Count = 0, id2Count = 0, id3Count = 0, idBaCount = 0, unknownCount = 0;
  for (uint8_t i = 0; i < capturedCount; i++) {
    if (lengths[i] == 0) { unknownCount++; continue; }
    switch (captured[i][0]) {
      case 0x01: id1Count++; break;
      case 0x02: id2Count++; break;
      case 0x03: id3Count++; break;
      case 0xBA: idBaCount++; break;
      default: unknownCount++; break;
    }
  }

  Serial.println();
  Serial.println("========================================");
  Serial.println(" ALL-INPUT TEST SUMMARY");
  Serial.println("========================================");
  Serial.print("Polls: "); Serial.println(pollCount);
  Serial.print("ACK reports: "); Serial.println(ackCount);
  Serial.print("NAK: "); Serial.println(nakCount);
  Serial.print("Errors: "); Serial.println(errorCount);
  Serial.print("Captured reports: "); Serial.println(capturedCount);
  Serial.print("Report ID 1 (Mouse): "); Serial.println(id1Count);
  Serial.print("Report ID 2 (System): "); Serial.println(id2Count);
  Serial.print("Report ID 3 (Consumer): "); Serial.println(id3Count);
  Serial.print("Report ID BA (Vendor): "); Serial.println(idBaCount);
  Serial.print("Unknown IDs: "); Serial.println(unknownCount);
  if (ackCount > capturedCount)
    Serial.println("WARNING: capture buffer full; some ACK reports were not stored.");
  if (errorCount) {
    Serial.print("Last HOSTSTATUS=0x"); printHexByte(lastErrorStatus); Serial.println();
  }

  if (capturedCount) {
    Serial.println();
    Serial.println("DECODED INPUT REPORTS");
    Serial.println("---------------------");
    for (uint8_t i = 0; i < capturedCount; i++) {
      mousePrintDetailedReport(captured[i], lengths[i],
                               (uint8_t)(i + 1), stages[i], toggles[i]);
    }
  }

  Serial.println();
  Serial.println("No vendor-specific OUT report was sent.");
  Serial.println("No SET_CONFIGURATION reset or device reset was issued after capture.");
  Serial.println("Run POSEIDON before starting a fresh ENUM18 sequence.");
  Serial.println("========================================");
}

// ==========================================
// FULL HID INPUT TEST WITH PER-STAGE BUFFERS
// ==========================================
// This is a second-generation capture command. It keeps the original
// MOUSEINPUTS command intact and adds MOUSEINPUTS2 with independent buffers
// for each physical test stage.
//
// Memory plan for Arduino Due (SAM3X, 96 KiB SRAM):
//   Mouse reports:   6 stages * 768 records * 9 bytes  = 41,472 bytes
//   Special reports: 6 stages *  64 records * 33 bytes = 12,672 bytes
//   Total capture storage:                              54,144 bytes
//   Per-stage counters/statistics add well under 1 KiB.
//
// Report ID 1 is exactly 8 bytes on the wire and gets a compact dedicated
// buffer. Other IDs (2, 3, 0xBA and unexpected IDs) get a 32-byte buffer.
// The largest descriptor-defined input report is ID 0xBA: 32 bytes.
// All ACK packets are counted even when a capture pool is full; overflow is
// explicitly reported. No vendor-specific OUT report is sent.

#define MOUSE_INPUTS2_STAGE_COUNT       6
#define MOUSE_INPUTS2_MOUSE_CAPACITY    768
#define MOUSE_INPUTS2_SPECIAL_CAPACITY  64
#define MOUSE_INPUTS2_MOUSE_LENGTH      8
#define MOUSE_INPUTS2_SPECIAL_LENGTH    32
#define MOUSE_INPUTS2_STAGE_MS          4000UL

struct MouseInputs2MouseRecord {
  // Bits 0..5 = length (always 8); bit 7 = DATA1, bit 6 reserved.
  uint8_t lengthAndToggle;
  uint8_t data[MOUSE_INPUTS2_MOUSE_LENGTH];
};

struct MouseInputs2SpecialRecord {
  // Bit 7 = DATA1; bits 0..6 = captured length (1..32).
  uint8_t lengthAndToggle;
  uint8_t data[MOUSE_INPUTS2_SPECIAL_LENGTH];
};

struct MouseInputs2Stage {
  MouseInputs2MouseRecord mouse[MOUSE_INPUTS2_MOUSE_CAPACITY];
  MouseInputs2SpecialRecord special[MOUSE_INPUTS2_SPECIAL_CAPACITY];

  uint16_t mouseStored;
  uint16_t specialStored;
  uint32_t polls;
  uint32_t ack;
  uint32_t nak;
  uint32_t errors;
  uint32_t droppedMouse;
  uint32_t droppedSpecial;
  uint32_t oversized;
  uint32_t zeroLength;

  uint32_t id1Count;
  uint32_t id2Count;
  uint32_t id3Count;
  uint32_t idBaCount;
  uint32_t unknownIdCount;

  uint32_t xReports;
  uint32_t yReports;
  uint32_t wheelReports;
  uint32_t panReports;
  int32_t sumX;
  int32_t sumY;
  int32_t sumWheel;
  int32_t sumPan;
  int32_t minX;
  int32_t maxX;
  int32_t minY;
  int32_t maxY;
  bool haveXY;

  uint8_t buttonsSeen;
  uint8_t previousButtons;
  bool havePreviousButtons;
  uint32_t buttonPressEdges[5];
  uint32_t buttonReleaseEdges[5];
};

static MouseInputs2Stage mouseInputs2Stages[MOUSE_INPUTS2_STAGE_COUNT];

void mouseInputs2ResetStage(MouseInputs2Stage &st) {
  // Clear counters only; report arrays are overwritten before they are read.
  st.mouseStored = 0;
  st.specialStored = 0;
  st.polls = 0;
  st.ack = 0;
  st.nak = 0;
  st.errors = 0;
  st.droppedMouse = 0;
  st.droppedSpecial = 0;
  st.oversized = 0;
  st.zeroLength = 0;
  st.id1Count = 0;
  st.id2Count = 0;
  st.id3Count = 0;
  st.idBaCount = 0;
  st.unknownIdCount = 0;
  st.xReports = 0;
  st.yReports = 0;
  st.wheelReports = 0;
  st.panReports = 0;
  st.sumX = 0;
  st.sumY = 0;
  st.sumWheel = 0;
  st.sumPan = 0;
  st.minX = 0;
  st.maxX = 0;
  st.minY = 0;
  st.maxY = 0;
  st.haveXY = false;
  st.buttonsSeen = 0;
  st.previousButtons = 0;
  st.havePreviousButtons = false;
  for (uint8_t i = 0; i < 5; i++) {
    st.buttonPressEdges[i] = 0;
    st.buttonReleaseEdges[i] = 0;
  }
}

void mouseInputs2Accumulate(MouseInputs2Stage &st,
                            const uint8_t *data,
                            uint8_t length) {
  if (length == 0) {
    st.zeroLength++;
    return;
  }

  uint8_t id = data[0];
  switch (id) {
    case 0x01: {
      st.id1Count++;
      if (length != 8) return;

      uint8_t buttons = data[1] & 0x1F;
      st.buttonsSeen |= buttons;
      if (st.havePreviousButtons) {
        uint8_t pressed = (uint8_t)(buttons & (uint8_t)~st.previousButtons);
        uint8_t released = (uint8_t)(st.previousButtons & (uint8_t)~buttons);
        for (uint8_t bit = 0; bit < 5; bit++) {
          if (pressed & (1U << bit)) st.buttonPressEdges[bit]++;
          if (released & (1U << bit)) st.buttonReleaseEdges[bit]++;
        }
      }
      st.previousButtons = buttons;
      st.havePreviousButtons = true;

      int32_t x = mouseReadS16LE(&data[2]);
      int32_t y = mouseReadS16LE(&data[4]);
      int32_t wheel = mouseReadS8(data[6]);
      int32_t pan = mouseReadS8(data[7]);
      if (x != 0) { st.xReports++; st.sumX += x; }
      if (y != 0) { st.yReports++; st.sumY += y; }
      if (wheel != 0) { st.wheelReports++; st.sumWheel += wheel; }
      if (pan != 0) { st.panReports++; st.sumPan += pan; }

      if (!st.haveXY) {
        st.minX = st.maxX = x;
        st.minY = st.maxY = y;
        st.haveXY = true;
      } else {
        if (x < st.minX) st.minX = x;
        if (x > st.maxX) st.maxX = x;
        if (y < st.minY) st.minY = y;
        if (y > st.maxY) st.maxY = y;
      }
      break;
    }
    case 0x02: st.id2Count++; break;
    case 0x03: st.id3Count++; break;
    case 0xBA: st.idBaCount++; break;
    default: st.unknownIdCount++; break;
  }
}

void mouseInputs2CapturePacket(MouseInputs2Stage &st,
                               bool data1,
                               uint8_t received) {
  uint8_t bytes[ MOUSE_INPUTS2_SPECIAL_LENGTH ];
  uint8_t copyLength = received;
  if (copyLength > MOUSE_INPUTS2_SPECIAL_LENGTH) {
    copyLength = MOUSE_INPUTS2_SPECIAL_LENGTH;
    st.oversized++;
  }

  for (uint8_t i = 0; i < copyLength; i++)
    bytes[i] = slReadRAM(ENUM_BUFFER_A + i);

  // Aggregate every ACKed report, even if storage is exhausted.
  mouseInputs2Accumulate(st, bytes, copyLength);

  if (copyLength == MOUSE_INPUTS2_MOUSE_LENGTH && bytes[0] == 0x01) {
    if (st.mouseStored < MOUSE_INPUTS2_MOUSE_CAPACITY) {
      MouseInputs2MouseRecord &rec = st.mouse[st.mouseStored++];
      rec.lengthAndToggle = (uint8_t)(copyLength | (data1 ? 0x80 : 0x00));
      for (uint8_t i = 0; i < MOUSE_INPUTS2_MOUSE_LENGTH; i++)
        rec.data[i] = bytes[i];
    } else {
      st.droppedMouse++;
    }
  } else {
    if (st.specialStored < MOUSE_INPUTS2_SPECIAL_CAPACITY) {
      MouseInputs2SpecialRecord &rec = st.special[st.specialStored++];
      rec.lengthAndToggle = (uint8_t)(copyLength | (data1 ? 0x80 : 0x00));
      for (uint8_t i = 0; i < copyLength; i++) rec.data[i] = bytes[i];
    } else {
      st.droppedSpecial++;
    }
  }
}

void mouseInputs2PollStage(uint8_t stageIndex,
                           const char *title,
                           const char *prompt,
                           bool &data1,
                           uint8_t &lastErrorStatus) {
  MouseInputs2Stage &st = mouseInputs2Stages[stageIndex];

  Serial.println();
  Serial.print("[STAGE "); Serial.print(stageIndex + 1);
  Serial.print("/"); Serial.print(MOUSE_INPUTS2_STAGE_COUNT);
  Serial.print("] "); Serial.println(title);
  Serial.println(prompt);
  Serial.println("Polling for 4 seconds; no serial output during the capture window.");

  uint32_t startMs = millis();
  uint32_t lastPollUs = 0;
  while ((uint32_t)(millis() - startMs) < MOUSE_INPUTS2_STAGE_MS) {
    if (lastPollUs != 0) {
      while ((uint32_t)(micros() - lastPollUs) <
             (uint32_t)MOUSE_INPUTS_INTERVAL_MS * 1000UL) {
        delayMicroseconds(50);
      }
    }
    lastPollUs = micros();
    st.polls++;

    EnumTransferResult result = enumIssueChannelAEndpoint(
      ENUM_PID_IN, MOUSE_INPUTS_ENDPOINT, false, data1,
      ENUM_NEW_DEVICE_ADDRESS, ENUM_BUFFER_A,
      MOUSE_INPUTS_MAX_PACKET, NULL);

    if (!result.started || !result.done) {
      st.errors++;
      lastErrorStatus = result.hostStatus;
      break;
    }
    if (result.hostStatus & ENUM_HOST_NAK) {
      st.nak++;
      continue; // No data packet: do not advance the DATA toggle.
    }
    if (!result.ack || !result.sequenceOK ||
        result.txLeft > MOUSE_INPUTS_MAX_PACKET) {
      st.errors++;
      lastErrorStatus = result.hostStatus;
      break;
    }

    st.ack++;
    uint8_t received = (uint8_t)(MOUSE_INPUTS_MAX_PACKET - result.txLeft);
    if (received == 0) {
      st.zeroLength++;
    } else {
      mouseInputs2CapturePacket(st, data1, received);
    }
    data1 = !data1; // An ACKed IN data packet consumes the current toggle.
  }

  Serial.print("Stage "); Serial.print(stageIndex + 1);
  Serial.print(" summary: polls="); Serial.print(st.polls);
  Serial.print(" ACK="); Serial.print(st.ack);
  Serial.print(" NAK="); Serial.print(st.nak);
  Serial.print(" errors="); Serial.println(st.errors);
  Serial.print("  Stored mouse="); Serial.print(st.mouseStored);
  Serial.print("/"); Serial.print(MOUSE_INPUTS2_MOUSE_CAPACITY);
  Serial.print(" special="); Serial.print(st.specialStored);
  Serial.print("/"); Serial.print(MOUSE_INPUTS2_SPECIAL_CAPACITY);
  Serial.print(" dropped mouse/special="); Serial.print(st.droppedMouse);
  Serial.print("/"); Serial.println(st.droppedSpecial);
}

void mouseInputs2PrintRaw(const uint8_t *data,
                          uint8_t length,
                          uint8_t stage,
                          uint16_t index,
                          bool data1) {
  Serial.print("S"); Serial.print(stage);
  Serial.print(" #"); Serial.print(index);
  Serial.print(" DATA"); Serial.print(data1 ? 1 : 0);
  Serial.print(" L="); Serial.print(length);
  Serial.print(" ID=");
  if (length) printHexByte(data[0]); else Serial.print("--");
  Serial.print(" RAW=");
  for (uint8_t i = 0; i < length; i++) {
    printHexByte(data[i]);
    if (i + 1 < length) Serial.print(' ');
  }

  if (length == 8 && data[0] == 0x01) {
    uint8_t b = data[1] & 0x1F;
    Serial.print(" | B="); printHexByte(b);
    Serial.print(" X="); Serial.print(mouseReadS16LE(&data[2]));
    Serial.print(" Y="); Serial.print(mouseReadS16LE(&data[4]));
    Serial.print(" W="); Serial.print(mouseReadS8(data[6]));
    Serial.print(" PAN="); Serial.print(mouseReadS8(data[7]));
  } else if (length == 2 && data[0] == 0x02) {
    Serial.print(" | SYSTEM=0x"); printHexByte(data[1]);
  } else if (length == 3 && data[0] == 0x03) {
    uint16_t usage = (uint16_t)data[1] | ((uint16_t)data[2] << 8);
    Serial.print(" | CONSUMER=0x");
    printHexByte((uint8_t)(usage >> 8)); printHexByte((uint8_t)usage);
  }
  Serial.println();
}

void mouseInputs2PrintStageData(uint8_t stageIndex) {
  MouseInputs2Stage &st = mouseInputs2Stages[stageIndex];
  Serial.println();
  Serial.print("========== CAPTURE DATA: STAGE ");
  Serial.print(stageIndex + 1); Serial.println(" ==========");

  for (uint16_t i = 0; i < st.mouseStored; i++) {
    MouseInputs2MouseRecord &rec = st.mouse[i];
    uint8_t length = rec.lengthAndToggle & 0x3F;
    bool data1 = (rec.lengthAndToggle & 0x80) != 0;
    mouseInputs2PrintRaw(rec.data, length, (uint8_t)(stageIndex + 1),
                         (uint16_t)(i + 1), data1);
  }
  for (uint16_t i = 0; i < st.specialStored; i++) {
    MouseInputs2SpecialRecord &rec = st.special[i];
    uint8_t length = rec.lengthAndToggle & 0x3F;
    bool data1 = (rec.lengthAndToggle & 0x80) != 0;
    mouseInputs2PrintRaw(rec.data, length, (uint8_t)(stageIndex + 1),
                         (uint16_t)(MOUSE_INPUTS2_MOUSE_CAPACITY + i + 1), data1);
  }
}

void mouseInputs2PrintSummary(uint8_t stageIndex) {
  MouseInputs2Stage &st = mouseInputs2Stages[stageIndex];
  Serial.println();
  Serial.print("STAGE "); Serial.print(stageIndex + 1); Serial.println(" RESULT");
  Serial.print("  ID1 mouse reports: "); Serial.println(st.id1Count);
  Serial.print("  ID2 system reports: "); Serial.println(st.id2Count);
  Serial.print("  ID3 consumer reports: "); Serial.println(st.id3Count);
  Serial.print("  ID BA vendor reports: "); Serial.println(st.idBaCount);
  Serial.print("  Unknown IDs: "); Serial.println(st.unknownIdCount);
  Serial.print("  Non-zero X/Y samples: ");
  Serial.print(st.xReports); Serial.print("/"); Serial.println(st.yReports);
  Serial.print("  Sum X/Y: "); Serial.print(st.sumX);
  Serial.print("/"); Serial.println(st.sumY);
  if (st.haveXY) {
    Serial.print("  X min/max: "); Serial.print(st.minX);
    Serial.print("/"); Serial.println(st.maxX);
    Serial.print("  Y min/max: "); Serial.print(st.minY);
    Serial.print("/"); Serial.println(st.maxY);
  }
  Serial.print("  Wheel samples/sum: ");
  Serial.print(st.wheelReports); Serial.print("/"); Serial.println(st.sumWheel);
  Serial.print("  Pan samples/sum: ");
  Serial.print(st.panReports); Serial.print("/"); Serial.println(st.sumPan);
  Serial.print("  Buttons ever seen (bits 0..4): 0x");
  printHexByte(st.buttonsSeen); Serial.println();
  for (uint8_t bit = 0; bit < 5; bit++) {
    Serial.print("  Button "); Serial.print(bit + 1);
    Serial.print(" press/release edges: ");
    Serial.print(st.buttonPressEdges[bit]); Serial.print("/");
    Serial.println(st.buttonReleaseEdges[bit]);
  }
  if (st.droppedMouse || st.droppedSpecial || st.oversized || st.zeroLength) {
    Serial.print("  Dropped mouse/special: ");
    Serial.print(st.droppedMouse); Serial.print("/"); Serial.println(st.droppedSpecial);
    Serial.print("  Oversized reports: "); Serial.println(st.oversized);
    Serial.print("  Zero-length reports: "); Serial.println(st.zeroLength);
  }
}

void poseidonMouseInputTests2() {
  Serial.println();
  Serial.println("========================================");
  Serial.println(" TRUST/YICHIP MOUSE - LARGE PER-STAGE CAPTURE");
  Serial.println("========================================");

  if (!enumAddress1Ready) {
    Serial.println("ERROR: Run POSEIDON and ENUM18 first.");
    return;
  }

  uint16_t vid = (uint16_t)enumStoredDeviceDescriptor[8] |
                 ((uint16_t)enumStoredDeviceDescriptor[9] << 8);
  uint16_t pid = (uint16_t)enumStoredDeviceDescriptor[10] |
                 ((uint16_t)enumStoredDeviceDescriptor[11] << 8);
  Serial.print("Device VID:PID = 0x");
  printHexByte((uint8_t)(vid >> 8)); printHexByte((uint8_t)vid);
  Serial.print(":0x");
  printHexByte((uint8_t)(pid >> 8)); printHexByte((uint8_t)pid);
  Serial.println();

  if (vid != 0x145F || pid != 0x02CF) {
    Serial.println("STOP: This diagnostic expects Trust/YICHIP VID:PID 145F:02CF.");
    return;
  }

  // Capture arrays are static/global to avoid consuming the 680-byte+ stack
  // footprint of the capture data. Only counters are reset here.
  for (uint8_t i = 0; i < MOUSE_INPUTS2_STAGE_COUNT; i++)
    mouseInputs2ResetStage(mouseInputs2Stages[i]);

  Serial.println("Capture allocation:");
  Serial.print("  Mouse reports per stage: "); Serial.println(MOUSE_INPUTS2_MOUSE_CAPACITY);
  Serial.print("  Special reports per stage: "); Serial.println(MOUSE_INPUTS2_SPECIAL_CAPACITY);
  Serial.print("  Static capture RAM: ");
  Serial.print((unsigned long)sizeof(mouseInputs2Stages)); Serial.println(" bytes");
  Serial.println("  Report ID 1 is stored in an 8-byte dedicated pool;");
  Serial.println("  other report IDs are stored in a 32-byte special pool.");

  if (!enumControlNoData(0x00, 0x09, 1, 0, "SET_CONFIGURATION(1)")) {
    Serial.println("STOP: SET_CONFIGURATION failed.");
    return;
  }
  if (!enumControlNoData(0x21, 0x0B, 1, MOUSE_INPUTS_INTERFACE,
                         "SET_PROTOCOL(Report=1), interface 1")) {
    Serial.println("STOP: SET_PROTOCOL(Report) failed.");
    return;
  }

  uint8_t protocol = 0xFF;
  uint16_t protocolLength = 0;
  if (!enumControlIn(0xA1, 0x03, 0, MOUSE_INPUTS_INTERFACE, 1,
                     &protocol, 1, protocolLength,
                     "GET_PROTOCOL after SET_PROTOCOL(Report)") ||
      protocolLength != 1 || protocol != 1) {
    Serial.println("STOP: Report Protocol was not confirmed.");
    return;
  }
  Serial.println("Report Protocol confirmed.");

  bool idleOK = enumControlNoData(0x21, 0x0A, 0x0000,
                                  MOUSE_INPUTS_INTERFACE,
                                  "SET_IDLE(0), all report IDs");
  Serial.print("SET_IDLE(0): "); Serial.println(idleOK ? "ACK" : "FAILED / STALL");

  bool data1 = false; // Endpoint 2's DATA toggle begins at DATA0 after configuration.
  uint8_t lastErrorStatus = 0;

  Serial.println();
  Serial.println("Six physical tests; each stage has its own independent capture pool.");
  Serial.println("Perform the requested action during its 4-second window.");

  mouseInputs2PollStage(0, "Baseline X/Y movement",
    "Move the mouse steadily in circles; do not press buttons.", data1, lastErrorStatus);
  mouseInputs2PollStage(1, "Left button",
    "Press and release the LEFT button several times.", data1, lastErrorStatus);
  mouseInputs2PollStage(2, "Right button",
    "Press and release the RIGHT button several times.", data1, lastErrorStatus);
  mouseInputs2PollStage(3, "Middle button",
    "Press and release the WHEEL/MIDDLE button several times.", data1, lastErrorStatus);
  mouseInputs2PollStage(4, "Vertical wheel",
    "Scroll UP and DOWN repeatedly; do not move the mouse if possible.", data1, lastErrorStatus);
  mouseInputs2PollStage(5, "DPI switch and movement",
    "Change DPI setting, then move the same physical distance; repeat for another setting.",
    data1, lastErrorStatus);

  uint32_t totalPolls = 0, totalAck = 0, totalNak = 0, totalErrors = 0;
  uint32_t totalStoredMouse = 0, totalStoredSpecial = 0;
  uint32_t totalDroppedMouse = 0, totalDroppedSpecial = 0;
  for (uint8_t i = 0; i < MOUSE_INPUTS2_STAGE_COUNT; i++) {
    MouseInputs2Stage &st = mouseInputs2Stages[i];
    totalPolls += st.polls;
    totalAck += st.ack;
    totalNak += st.nak;
    totalErrors += st.errors;
    totalStoredMouse += st.mouseStored;
    totalStoredSpecial += st.specialStored;
    totalDroppedMouse += st.droppedMouse;
    totalDroppedSpecial += st.droppedSpecial;
  }

  Serial.println();
  Serial.println("========================================");
  Serial.println(" MOUSEINPUTS2 TOTAL SUMMARY");
  Serial.println("========================================");
  Serial.print("Polls: "); Serial.println(totalPolls);
  Serial.print("ACK reports: "); Serial.println(totalAck);
  Serial.print("NAK: "); Serial.println(totalNak);
  Serial.print("Controller errors: "); Serial.println(totalErrors);
  Serial.print("Stored mouse reports: "); Serial.println(totalStoredMouse);
  Serial.print("Stored special reports: "); Serial.println(totalStoredSpecial);
  Serial.print("Dropped mouse/special: ");
  Serial.print(totalDroppedMouse); Serial.print("/"); Serial.println(totalDroppedSpecial);
  if (totalErrors) {
    Serial.print("Last HOSTSTATUS=0x"); printHexByte(lastErrorStatus); Serial.println();
  }

  for (uint8_t i = 0; i < MOUSE_INPUTS2_STAGE_COUNT; i++)
    mouseInputs2PrintSummary(i);

  Serial.println();
  Serial.println("========================================");
  Serial.println(" CAPTURED REPORT DATA");
  Serial.println("========================================");
  for (uint8_t i = 0; i < MOUSE_INPUTS2_STAGE_COUNT; i++)
    mouseInputs2PrintStageData(i);

  Serial.println();
  Serial.println("No vendor-specific OUT report was sent.");
  Serial.println("All six capture pools are independent; overflow is counted per stage.");
  Serial.println("Run POSEIDON before starting another fresh ENUM18 sequence.");
  Serial.println("========================================");
}

// ==========================================
// USB TRANSFER
// ==========================================

#define USB_DONE       0x01
#define USB_ACK        0x01
#define USB_TIMEOUT    0x04
#define USB_STALL      0x80
#define USB_ERROR      0x02
#define USB_OVERFLOW   0x20

bool waitUSB(uint16_t timeoutMs) {

  uint32_t start = millis();

  while (millis() - start < timeoutMs) {

    uint8_t status = slReadRegister(REG_INTSTATUS);

    if (status & USB_DONE) {
      slWriteRegister(REG_INTSTATUS, USB_DONE);
      return true;
    }

    delayMicroseconds(100);
  }

  return false;
}

bool checkPacket(const char *name) {

  uint8_t status = slReadRegister(REG_PID);
  uint8_t count  = slReadRegister(REG_ADDR);

  Serial.print(name);
  Serial.print(" STATUS=0x");
  Serial.print(status, HEX);
  Serial.print(" COUNT=");
  Serial.println(count);

  if (status & USB_STALL) {
    Serial.println("USB STALL");
    return false;
  }

  if (status & USB_TIMEOUT) {
    Serial.println("USB TIMEOUT");
    return false;
  }

  if (status & USB_ERROR) {
    Serial.println("USB ERROR");
    return false;
  }

  if (status & USB_OVERFLOW) {
    Serial.println("USB OVERFLOW");
    return false;
  }

  if (!(status & USB_ACK)) {
    Serial.println("No ACK");
    return false;
  }

  return true;
}

// ==========================================
// SETUP TRANSACTION
// ==========================================

bool usbSetup(uint8_t *setup) {

  Serial.println("Sending SETUP...");

  for (int i = 0; i < 8; i++)
    slWriteRAM(0x10 + i, setup[i]);

  slWriteRegister(REG_BASE, 0x10);
  slWriteRegister(REG_LENGTH, 8);
  slWriteRegister(REG_ADDR, 0x00);
  slWriteRegister(REG_PID, 0xD0);

  slWriteRegister(REG_INTSTATUS, USB_DONE);

  // OUT + ENABLE + ARM, SETUP PID
  slWriteRegister(REG_CTRL, 0x07);

  if (!waitUSB(500)) {
    Serial.println("SETUP: no DONE interrupt");
    return false;
  }

  return checkPacket("SETUP");
}

// ==========================================
// IN DATA PACKET
// ==========================================

bool usbInPacket(uint8_t buffer,
                 uint8_t requested,
                 bool data1,
                 uint8_t &received) {

  slWriteRegister(REG_BASE, buffer);
  slWriteRegister(REG_LENGTH, requested);
  slWriteRegister(REG_ADDR, 0x00);
  slWriteRegister(REG_PID, 0x90);

  slWriteRegister(REG_INTSTATUS, USB_DONE);

  // DATA toggle + ENABLE + ARM
  uint8_t control = data1 ? 0x43 : 0x03;

  slWriteRegister(REG_CTRL, control);

  if (!waitUSB(500)) {
    Serial.println("IN: no DONE interrupt");
    return false;
  }

  if (!checkPacket("IN"))
    return false;

  received = slReadRegister(REG_ADDR);

  if (received > requested) {
    Serial.println("Invalid received length");
    return false;
  }

  return true;
}

// ==========================================
// USB STATUS OUT
// ==========================================

bool usbStatusOut() {

  Serial.println("Sending control STATUS OUT...");

  slWriteRegister(REG_BASE, 0x10);
  slWriteRegister(REG_LENGTH, 0);
  slWriteRegister(REG_ADDR, 0x00);
  slWriteRegister(REG_PID, 0x10);

  slWriteRegister(REG_INTSTATUS, USB_DONE);

  // OUT + DATA1 + ENABLE + ARM
  slWriteRegister(REG_CTRL, 0x47);

  if (!waitUSB(500)) {
    Serial.println("STATUS OUT: no DONE");
    return false;
  }

  return checkPacket("STATUS OUT");
}

// ==========================================
// GET DEVICE DESCRIPTOR
// ==========================================

bool getDescriptor(uint8_t requestedLength,
                   uint8_t *descriptor,
                   uint8_t maxPacket) {

  uint8_t setup[8] = {
    0x80, 0x06, 0x00, 0x01,
    0x00, 0x00, requestedLength, 0x00
  };

  if (!usbSetup(setup))
    return false;

  uint8_t total = 0;
  bool toggle = true;

  while (total < requestedLength) {

    uint8_t remaining = requestedLength - total;
    uint8_t packetSize = min(remaining, maxPacket);

    uint8_t received = 0;

    if (!usbInPacket(0x10, packetSize,
                     toggle, received)) {
      Serial.println("Descriptor IN failed");
      return false;
    }

    Serial.print("Received packet: ");
    Serial.print(received);
    Serial.println(" bytes");

    for (uint8_t i = 0; i < received; i++) {
      descriptor[total + i] =
        slReadRAM(0x10 + i);
    }

    total += received;

    if (received < packetSize)
      break;

    toggle = !toggle;

    if (received == 0)
      break;
  }

  Serial.print("Total descriptor bytes: ");
  Serial.println(total);

  if (total != requestedLength) {
    Serial.println("Incomplete descriptor!");
    return false;
  }

  return usbStatusOut();
}

// ==========================================
// MOUSE IDENTIFICATION
// ==========================================

void usbMouseTest() {

  uint8_t descriptor[18];

  Serial.println();
  Serial.println("================================");
  Serial.println(" USB MOUSE DONGLE IDENTIFICATION");
  Serial.println("================================");

  usbInit();

  // Detect connection state
  uint8_t port = slReadRegister(REG_INTSTATUS);

  Serial.print("Port status: 0x");
  Serial.println(port, HEX);

  if (!(port & 0x80)) {
    Serial.println("WARNING: D+ is not HIGH.");
    Serial.println("This may be a Low-Speed device.");
    Serial.println("This test currently uses Full-Speed.");
    return;
  }

  usbResetDevice();

  // First descriptor request: 8 bytes
  Serial.println();
  Serial.println("Reading first 8 descriptor bytes...");

  if (!getDescriptor(8, descriptor, 8)) {
    Serial.println("First descriptor request FAILED.");
    return;
  }

  uint8_t maxPacket = descriptor[7];

  Serial.print("EP0 maximum packet size: ");
  Serial.println(maxPacket);

  if (maxPacket != 8 &&
      maxPacket != 16 &&
      maxPacket != 32 &&
      maxPacket != 64) {
    Serial.println("Invalid EP0 packet size.");
    return;
  }

  // Full descriptor request
  Serial.println();
  Serial.println("Reading complete 18-byte descriptor...");

  if (!getDescriptor(18, descriptor, maxPacket)) {
    Serial.println("Full descriptor request FAILED.");
    return;
  }

  uint16_t vid =
    (uint16_t)descriptor[8] |
    ((uint16_t)descriptor[9] << 8);

  uint16_t pid =
    (uint16_t)descriptor[10] |
    ((uint16_t)descriptor[11] << 8);

  Serial.println();
  Serial.println("================================");
  Serial.println(" USB DEVICE IDENTIFIED");
  Serial.println("================================");

  Serial.print("VID: 0x");
  if (vid < 0x1000) Serial.print("0");
  Serial.println(vid, HEX);

  Serial.print("PID: 0x");
  if (pid < 0x1000) Serial.print("0");
  Serial.println(pid, HEX);

  Serial.println();
  Serial.println("Complete Device Descriptor:");

  for (int i = 0; i < 18; i++) {
    if (i % 8 == 0)
      Serial.println();

    if (descriptor[i] < 0x10)
      Serial.print("0");

    Serial.print(descriptor[i], HEX);
    Serial.print(" ");
  }

  Serial.println();
  Serial.println("================================");
}

// ==========================================
// COMMAND PROCESSING
// ==========================================

void printHelp() {

  Serial.println();
  Serial.println("SL811HS COMMANDS");
  Serial.println("----------------");
  Serial.println("R XX       - Read register");
  Serial.println("W XX YY    - Write register");
  Serial.println("RESET      - Hardware reset");
  Serial.println("MOUSE      - Read USB VID/PID");
  Serial.println("POSEIDON   - Run Poseidon initialization sequence");
  Serial.println("ENUM8      - SETUP + first IN for 8-byte device descriptor (no reinit)");
  Serial.println("ENUM18     - full Device Descriptor + STATUS OUT + SET_ADDRESS + address-1 check");
  Serial.println("IDENTIFY   - read USB strings + all configuration/interface/endpoint descriptors (after ENUM18)");
  Serial.println("MSDINQUIRY - discover Mass Storage BOT interface and run SCSI INQUIRY (read-only)");
  Serial.println("MSDREAD    - TEST UNIT READY (retry medium-change UNIT ATTENTION), READ CAPACITY(10), READ(10) LBA 0");
  Serial.println("MSDWRITE   - WRITE(10) one scratch block, READ(10) back and compare every byte (destructive test) ");
  Serial.println("MSDRDB     - scan LBA 0..15 for Amiga RDB signature RDSK (read-only)");
  Serial.println("MSDRDBINFO - follow and decode RDB PART + FSHD linked lists (read-only)");
  Serial.println("MOUSEIRQ   - basic SET_CONFIGURATION + Boot Protocol + Interrupt IN test");
  Serial.println("MOUSEIRQ2  - GET_PROTOCOL + GET_REPORT + SET_IDLE(20 ms) + Interrupt IN test");
  Serial.println("MOUSEFEATURES - fetch/decode HID Report Descriptor, switch to Report Protocol, capture raw reports");
  Serial.println("MOUSEINPUTS - original compact staged HID input diagnostic");
  Serial.println("MOUSEINPUTS2 - six independent large capture buffers; X/Y, 3 buttons, wheel and DPI tests");
  Serial.println("HELP       - Show help");
  Serial.println();
}

void processCommand(char *cmd) {

  char *token = strtok(cmd, " \t");

  if (!token) return;

  if (!strcmp(token, "R") ||
      !strcmp(token, "r")) {

    char *arg = strtok(NULL, " \t");

    if (!arg) {
      Serial.println("ERROR: Missing address");
      return;
    }

    uint8_t address = strtoul(arg, NULL, 16);
    uint8_t value = slReadRegister(address);

    Serial.print("Register 0x");
    Serial.print(address, HEX);
    Serial.print(" = 0x");

    if (value < 16) Serial.print("0");

    Serial.println(value, HEX);
    return;
  }

  if (!strcmp(token, "W") ||
      !strcmp(token, "w")) {

    char *argAddress = strtok(NULL, " \t");
    char *argValue = strtok(NULL, " \t");

    if (!argAddress || !argValue) {
      Serial.println("ERROR: Use W address value");
      return;
    }

    uint8_t address = strtoul(argAddress, NULL, 16);
    uint8_t value = strtoul(argValue, NULL, 16);

    slWriteRegister(address, value);

    Serial.print("Written 0x");
    Serial.print(value, HEX);
    Serial.print(" to register 0x");
    Serial.println(address, HEX);

    return;
  }

  if (!strcmp(token, "RESET") ||
      !strcmp(token, "reset")) {

    slReset();
    Serial.println("SL811HS hardware reset done");
    return;
  }

  if (!strcmp(token, "MOUSE") ||
      !strcmp(token, "mouse")) {

    usbMouseTest();
    return;
  }

  if (!strcmp(token, "POSEIDON") ||
      !strcmp(token, "poseidon")) {

    poseidonInitialization();
    return;
  }

  if (!strcmp(token, "ENUM8") ||
      !strcmp(token, "enum8")) {

    poseidonEnum8();
    return;
  }

  if (!strcmp(token, "ENUM18") ||
      !strcmp(token, "enum18")) {

    poseidonEnum18SetAddress();
    return;
  }

  if (!strcmp(token, "MOUSEIRQ") ||
      !strcmp(token, "mouseirq")) {

    poseidonMouseInterruptTest();
    return;
  }

  if (!strcmp(token, "MOUSEIRQ2") ||
      !strcmp(token, "mouseirq2")) {

    poseidonMouseInterruptDiagnostics();
    return;
  }

  if (!strcmp(token, "MOUSEFEATURES") ||
      !strcmp(token, "mousefeatures")) {

    poseidonMouseFeatures();
    return;
  }

  if (!strcmp(token, "MOUSEINPUTS") ||
      !strcmp(token, "mouseinputs")) {

    poseidonMouseInputTests();
    return;
  }

  if (!strcmp(token, "MOUSEINPUTS2") ||
      !strcmp(token, "mouseinputs2")) {

    poseidonMouseInputTests2();
    return;
  }

  if (!strcmp(token, "IDENTIFY") ||
      !strcmp(token, "identify")) {

    poseidonIdentify();
    return;
  }

  if (!strcmp(token, "MSDINQUIRY") ||
      !strcmp(token, "msdinquiry") ||
      !strcmp(token, "MSD") ||
      !strcmp(token, "msd")) {

    poseidonMassStorageInquiry();
    return;
  }

  if (!strcmp(token, "MSDREAD") ||
      !strcmp(token, "msdread") ||
      !strcmp(token, "MSDSECTOR") ||
      !strcmp(token, "msdsector")) {

    poseidonMassStorageReadFirstSector();
    return;
  }

  if (!strcmp(token, "MSDWRITE") ||
      !strcmp(token, "msdwrite") ||
      !strcmp(token, "MSDWRITETEST") ||
      !strcmp(token, "msdwritetest")) {

    poseidonMassStorageWriteVerifyTest();
    return;
  }

  if (!strcmp(token, "MSDRDB") ||
      !strcmp(token, "msdrdb") ||
      !strcmp(token, "RDB") ||
      !strcmp(token, "rdb")) {

    poseidonMassStorageFindRdb();
    return;
  }

  if (!strcmp(token, "MSDRDBINFO") ||
      !strcmp(token, "msdrdbinfo") ||
      !strcmp(token, "RDBINFO") ||
      !strcmp(token, "rdbinfo")) {

    poseidonMassStorageRdbDetails();
    return;
  }

  if (!strcmp(token, "HELP") ||
      !strcmp(token, "help")) {

    printHelp();
    return;
  }

  Serial.println("Unknown command. Type HELP.");
}

// ==========================================
// SETUP
// ==========================================

void setup() {

  Serial.begin(115200);
  delay(500);

  pinMode(PIN__SPARE_CS, OUTPUT);
  pinMode(PIN__IORD, OUTPUT);
  pinMode(PIN__IOWR, OUTPUT);
  pinMode(PIN_A2, OUTPUT);
  pinMode(PIN__RESET, OUTPUT);

  pinMode(PIN__INT6, INPUT);

  digitalWrite(PIN__SPARE_CS, HIGH);
  digitalWrite(PIN__IORD, HIGH);
  digitalWrite(PIN__IOWR, HIGH);
  digitalWrite(PIN_A2, HIGH);
  digitalWrite(PIN__RESET, HIGH);

  dataBusInput();

  slReset();

  Serial.println();
  Serial.println("================================");
  Serial.println(" SL811HS INTERACTIVE TESTER");
  Serial.println("================================");
  Serial.println("Ready. Type HELP.");
}

// ==========================================
// MAIN LOOP
// ==========================================

void loop() {

  while (Serial.available()) {

    char c = Serial.read();

    if (c == '\r') continue;

    if (c == '\n') {

      commandBuffer[commandPos] = '\0';

      if (commandPos > 0)
        processCommand(commandBuffer);

      commandPos = 0;
    }
    else if (commandPos < sizeof(commandBuffer) - 1) {
      commandBuffer[commandPos++] = c;
    }
  }
}

