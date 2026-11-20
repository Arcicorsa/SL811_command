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
  uint8_t attempts;       // Number of channel-A transfers actually issued
  uint8_t nakCount;       // Number of NAK responses that triggered retry handling
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

EnumTransferResult enumIssueChannelA(uint8_t pid,
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
  slWriteRegister(REG_PID, (uint8_t)((pid << 4) | ENUM_EP0));
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
  Serial.print(result.attempts);
  Serial.print(" / ");
  Serial.println(ENUM_MAX_RETRIES);
  Serial.print("NAK responses: ");
  Serial.println(result.nakCount);

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

  if (!strcmp(token, "IDENTIFY") ||
      !strcmp(token, "identify")) {

    poseidonIdentify();
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

  Serial.begin(9600);
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

