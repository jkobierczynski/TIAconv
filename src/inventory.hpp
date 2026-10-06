// tiaconv - TIA Portal project reader
// Copyright (C) 2026 Jurgen Kobierczynski
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Hardware and network inventory: devices, the modules in them, their
// interfaces and addresses, and the subnets that connect them.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "project.hpp"

namespace tia {

struct Interface {
    std::string name;       // node name, e.g. "X1 : PN(LAN)"
    std::string item;       // device item the node belongs to, e.g. "PROFINET interface_1"
    std::string nodeId;
    int64_t nodeType = 0;
    bool hasIpSettings = false;
    std::string ip, mask, router;
    bool ipProtocolUsed = false;
    // NodeIPConfiguration is not 0: the address is assigned outside the
    // engineering project (for example set directly at the device), so `ip`
    // may not be what the device uses.
    bool ipAssignedElsewhere = false;
    int64_t ipConfiguration = 0;  // raw NodeIPConfiguration value
    // NodeIPAddresseSetByUser. In the V21 test projects it is false until the
    // user types an address. The V13 sample has it false on a non-default
    // address, so it is reported as data only and not interpreted.
    bool hasIpSetByUser = false;
    bool ipSetByUser = false;
    bool hasBusAddress = false;   // PROFIBUS / MPI station address
    int64_t busAddress = 0;
    // PROFINET device name. With profinetNameAuto TIA Portal derives the name
    // from the device name and the stored value is not kept up to date, so it
    // goes to profinetNameStored and profinetName stays empty.
    std::string profinetName;
    std::string profinetNameStored;
    bool profinetNameAuto = false;
    std::string configuredMac;
    std::string subnet;
};

// A yes/no setting. A project stores most settings only once they have been
// changed from TIA Portal's default, so "not stored" is a state of its own:
// the default of that CPU and firmware applies, whatever it is.
struct Setting {
    bool stored = false;
    bool on = false;
};

// Security-relevant settings of a controller, collected from the controller
// object and the items below it (interfaces, display, OPC UA).
struct Security {
    // ProtectionLevel. The meaning of the number depends on the CPU family,
    // see accessLevelName; the name is empty where it has not been verified.
    bool hasAccessLevel = false;
    int64_t accessLevel = 0;
    std::string accessLevelName;
    Setting putGet;                        // access via PUT/GET from remote partners
    Setting webServer;                     // web server activated on the module
    Setting webServerHttpsOnly;
    std::vector<std::string> webServerInterfaces;  // interfaces with web server access switched on
    Setting opcUaServer;
    Setting displayProtection;             // password on the CPU display
    // The CPU has user management: access is decided by users and roles,
    // which the project stores in protected form and tiaconv does not read.
    bool userManagement = false;
    // TIA Portal's name for the set of user rights this CPU knows. Older
    // CPUs have one too (OPC UA users) without having user management.
    std::string functionRightSet;
    Setting accessControl;                 // user-based access control (current CPUs)
    Setting accessControlViaAccessLevels;  // ... with the older access levels and passwords
    Setting configDataProtection;          // protection of confidential PLC configuration data
    // OmsCommunicationMode: 0 = legacy PG/PC and HMI communication is permitted
    // next to the secure one. Other values have not been seen.
    bool hasCommunicationMode = false;
    int64_t communicationMode = 0;
    // TimeSyncRole on an interface: 2 = time synchronisation via NTP.
    bool hasTimeSyncRole = false;
    int64_t timeSyncRole = 0;
    std::vector<std::string> ntpServers;

    bool empty() const;
};

// What decides who may access the CPU, as far as the project says:
//   "access_levels"    the access level and its passwords (CPUs without user management)
//   "users_and_roles"  users and roles, which are not read
//   "users_and_roles_and_access_levels"  both
//   "none"             access control is disabled
std::string accessProtection(const Security& s);

// Name of an access level for a CPU type ("S71500.CPU") and firmware ("V1.8");
// empty for combinations that have not been checked against TIA Portal.
std::string accessLevelName(const std::string& type, const std::string& firmware, int64_t level);

struct Module {
    uint64_t id = 0;
    std::string name;
    std::string kind;        // controller, rack, module, submodule, port, interface, item
    std::string type;        // e.g. "S71200.CPU"
    std::string typeName;    // e.g. "CPU 1215C DC/DC/DC"
    std::string orderNumber;
    std::string firmware;
    bool hasPosition = false;
    int64_t position = 0;
    int64_t itemType = 0;    // raw DeviceItemType bit field
    std::string container;   // name of the item this one is plugged into
    std::string author;
    std::string modified;
    std::vector<Interface> interfaces;
    Security security;       // controllers only
    std::string ioController;  // IO devices: the controller they are assigned to
    std::string ioSystem;      // ... and its IO system
};

struct Device {
    uint64_t id = 0;
    std::string name;
    std::string type;
    bool inProject = false;   // attached directly to the project (not a library copy)
    std::string parentType;   // type of the object the device hangs under
    std::vector<Module> modules;
};

struct SubnetMember {
    std::string device, module, node, ip;
};

struct Subnet {
    std::string name;
    int64_t netType = 0;
    std::vector<SubnetMember> members;
};

// One end of a configured connection, as the device that owns it sees it.
struct ConnectionEnd {
    std::string device, module, interface, ip;
};

// A connection configured in the project (not one opened by the program at
// run time with TCON and the like).
struct Connection {
    uint64_t id = 0;
    std::string name;
    std::string kind;            // "HMI", or the stored type name
    ConnectionEnd local;         // the side the connection is configured on
    ConnectionEnd partner;       // empty parts where the partner is not in the project
    // Both partners are in the project and each holds its half; the two halves
    // are reported as one connection, seen from the side that opens it.
    bool bothSides = false;
    bool hasPartnerId = false;
    int64_t partnerId = 0;       // connection ID on the partner, when bothSides
    std::string partnerAddress;  // address typed in for a partner outside the project
    bool hasLocalId = false;
    int64_t localId = 0;         // connection ID on the owning device
    Setting overTcpIp;           // S7 protocol carried over TCP/IP
    Setting oneWay;              // only the owning side sends requests
    Setting activeEstablishment; // the owning side opens the connection
};

// One device of an IO system.
struct IoDevice {
    std::string device, module, interface, ip;
};

// A PROFINET IO system (or other master system): one controller and the
// devices assigned to it.
struct IoSystem {
    uint64_t id = 0;
    std::string name;            // e.g. "PROFINET IO-System"
    std::string kind;            // stored type name, e.g. "IOSystem_PROFINET"
    bool hasNumber = false;
    int64_t number = 0;          // 100 for the first PROFINET IO system of a controller
    std::string controllerDevice, controller;
    std::string subnet;
    std::vector<IoDevice> devices;
};

// A cable between two ports, as drawn in the topology view.
struct PortEnd {
    std::string device, module, port;
};
struct PortLink {
    PortEnd a, b;
};

struct ProjectInfo {
    bool found = false;
    std::string name, created, modified, author, lastModifiedBy;
};

struct InventoryStats {
    size_t blocks = 0;
    size_t liveObjects = 0;
    size_t deletedObjects = 0;
    size_t saves = 0;  // completed saves recorded in the file
    size_t decodedObjects = 0;
    size_t objectsWithProblems = 0;
    size_t unattachedItems = 0;
};

struct Inventory {
    ProjectInfo project;
    std::vector<Device> devices;
    std::vector<Subnet> subnets;
    std::vector<IoSystem> ioSystems;
    std::vector<PortLink> portLinks;
    std::vector<Connection> connections;
    std::vector<std::string> saves;  // commit timestamps (older layout only)
    InventoryStats stats;
    std::vector<std::string> warnings;
};

struct InventoryOptions {
    bool allItems = false;  // keep ports and internal items
};

Inventory buildInventory(const Project& project, const InventoryOptions& opt = {});

}  // namespace tia
