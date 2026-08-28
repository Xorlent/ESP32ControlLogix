#pragma once

/*
 * ESP32ControlLogix - native, ground-up EtherNet/IP + CIP client for
 * Allen-Bradley/Rockwell ControlLogix and compatible Logix controllers.
 *
 * This is the public umbrella header. It exposes the full non-blocking stack:
 * Ethernet transport (Client), TCP (TcpConnection), EtherNet/IP session
 * (Session), CIP explicit and connected messaging (ExplicitMessage,
 * Connection), symbolic Logix tags (Tag), and the top-level PlcClient facade.
 *
 * All library calls are non-blocking: begin()/connect() start work and return
 * immediately; poll() advances state. No public call waits for network,
 * socket, or PLC progress.
 */

#include <stdint.h>

#define ESP32_CONTROLLOGIX_VERSION_MAJOR 0
#define ESP32_CONTROLLOGIX_VERSION_MINOR 1
#define ESP32_CONTROLLOGIX_VERSION_PATCH 3

#include "transport/Status.h"
#include "transport/Client.h"
#include "transport/TcpConnection.h"
#include "eip/Encapsulation.h"
#include "eip/Session.h"
#include "cip/Cip.h"
#include "cip/ExplicitMessage.h"
#include "cip/Connection.h"
#include "tag/Tag.h"
#include "PlcClient.h"

namespace clx {

// Library version string, e.g. "0.1.0".
const char *version();

}  // namespace clx
