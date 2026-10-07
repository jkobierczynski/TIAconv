// tiaconv - TIA Portal project reader
// Copyright (C) 2026 Jurgen Kobierczynski
// SPDX-License-Identifier: GPL-3.0-or-later
#include "inventory.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <map>

namespace tia {
namespace {

using Key = std::pair<uint32_t, uint64_t>;

enum class Role { None, Project, Device, Item, Node, Subnet, Connection, IoSystem, RightSet, Right, Persistence };

const char kBaseDevice[] = "Siemens.Automation.DomainModel.BaseDeviceData";
const char kBaseDeviceItem[] = "Siemens.Automation.DomainModel.BaseDeviceItemData";
const char kControllerTarget[] = "Siemens.Automation.DomainModel.ControllerTargetData";
const char kProject[] = "Siemens.Automation.DomainModel.ProjectData";
const char kNode[] = "Siemens.Simatic.HwConfiguration.Model.NodeData";
const char kSubnet[] = "Siemens.Simatic.HwConfiguration.Model.SubnetData";

// DeviceItemType bits, inferred from sample projects (see docs/FORMAT.md).
constexpr int64_t kItemRack = 0x1;
constexpr int64_t kItemModule = 0x2;
constexpr int64_t kItemSubmodule = 0x4;
constexpr int64_t kItemPort = 0x4000;

std::string ipv4(uint64_t v) {
    char buf[20];
    std::snprintf(buf, sizeof buf, "%u.%u.%u.%u", static_cast<unsigned>((v >> 24) & 255),
                  static_cast<unsigned>((v >> 16) & 255), static_cast<unsigned>((v >> 8) & 255),
                  static_cast<unsigned>(v & 255));
    return buf;
}

std::string macAddress(uint64_t v) {
    char buf[20];
    std::snprintf(buf, sizeof buf, "%02X-%02X-%02X-%02X-%02X-%02X", static_cast<unsigned>((v >> 40) & 255),
                  static_cast<unsigned>((v >> 32) & 255), static_cast<unsigned>((v >> 24) & 255),
                  static_cast<unsigned>((v >> 16) & 255), static_cast<unsigned>((v >> 8) & 255),
                  static_cast<unsigned>(v & 255));
    return buf;
}

int64_t intAttr(const Object& o, const char* set, const char* name, bool* found = nullptr) {
    const Value* v = o.attr(set, name);
    uint64_t u = 0;
    bool ok = v && v->asUInt(u);
    if (found) *found = ok;
    return ok ? static_cast<int64_t>(u) : 0;
}

std::string expandoIp(const Object& o, const char* name) {
    const Value* v = o.expandoValue(name);
    uint64_t u = 0;
    return (v && v->asUInt(u)) ? ipv4(u) : std::string();
}

void readSetting(const Object& o, const char* name, Setting& s) {
    if (const Value* v = o.expandoValue(name)) {
        if (v->isNull()) return;
        s.stored = true;
        s.on = v->truthy();
    }
}

bool readNumber(const Object& o, const char* name, int64_t& out) {
    const Value* v = o.expandoValue(name);
    uint64_t u = 0;
    if (!v || !v->asUInt(u)) return false;
    out = static_cast<int64_t>(u);
    return true;
}

// What one object says about the security settings of its controller.
struct ItemSecurity {
    Security s;
    Setting webAccess;  // EnableWebServerAccess4IE, on an interface item
};

ItemSecurity readSecurity(const Object& o) {
    ItemSecurity r;
    Security& s = r.s;
    s.hasAccessLevel = readNumber(o, "ProtectionLevel", s.accessLevel);
    readSetting(o, "EnablePutGetConnections", s.putGet);
    readSetting(o, "WebServerActive", s.webServer);
    readSetting(o, "WebServerSSLOnly", s.webServerHttpsOnly);
    readSetting(o, "OpcUaEnableServer", s.opcUaServer);
    readSetting(o, "EnableDisplayProtection", s.displayProtection);
    readSetting(o, "AccessControlAtRuntime", s.accessControl);
    readSetting(o, "EnableLegacyAccessControlViaAccessLevel", s.accessControlViaAccessLevels);
    // The option itself where it is stored; otherwise a configured password
    // for it means it is on.
    readSetting(o, "ProtectPlcConfiguration", s.configDataProtection);
    if (!s.configDataProtection.stored) {
        Setting secret;
        readSetting(o, "IsMasterSecretConfigured", secret);
        if (secret.stored && secret.on) s.configDataProtection = secret;
    }
    s.hasCommunicationMode = readNumber(o, "OmsCommunicationMode", s.communicationMode);
    s.hasTimeSyncRole = readNumber(o, "TimeSyncRole", s.timeSyncRole);
    for (const char* key : {"TimeSyncNtpServer1", "TimeSyncNtpServer2", "TimeSyncNtpServer3", "TimeSyncNtpServer4"})
        if (const Value* v = o.expandoValue(key))
            if (v->type == Value::Type::String && !v->s.empty()) s.ntpServers.push_back(v->s);
    readSetting(o, "EnableWebServerAccess4IE", r.webAccess);
    return r;
}

void take(Setting& into, const Setting& from) {
    if (from.stored) into = from;
}

struct ItemRec {
    Module module;
    ItemSecurity security;
    Key parent{0, 0};     // BaseDeviceItemData.Parent: the device or the enclosing item
    bool hasParent = false;
    Key container{0, 0};  // BaseDeviceItemData.Container: where it is plugged in
    bool hasContainer = false;
    bool controller = false;
    Key interfaceItem{0, 0};  // ports: the interface they belong to
    bool hasInterfaceItem = false;
    std::vector<Key> portPeers;  // ports: the ports they are cabled to
};

}  // namespace

bool Security::empty() const {
    return !hasAccessLevel && !putGet.stored && !webServer.stored && !webServerHttpsOnly.stored &&
           webServerInterfaces.empty() && !opcUaServer.stored && !displayProtection.stored && !accessControl.stored &&
           !accessControlViaAccessLevels.stored && !configDataProtection.stored && !hasCommunicationMode &&
           !hasTimeSyncRole && !userManagement;
}

// What decides who may access the CPU, as far as the project says.
//   access_levels    the access level and its passwords (CPUs without user management)
//   users_and_roles  users and roles, which tiaconv does not read
//   users_and_roles_and_access_levels  both
//   none             access control is disabled
std::string accessProtection(const Security& s) {
    if (s.accessControl.stored && !s.accessControl.on) return "none";
    if (!s.userManagement && !s.accessControl.stored) return "access_levels";
    return s.accessControlViaAccessLevels.stored && s.accessControlViaAccessLevels.on
               ? "users_and_roles_and_access_levels"
               : "users_and_roles";
}

// Checked against TIA Portal V21 for an S7-1500 with firmware V1.8 and V4.1,
// an S7-1200 with firmware V2.2 (tests/fixtures/s09_security) and an S7-1200
// with firmware V4.7 (s10_connections). The S7-1200 got the four S7-1500
// levels with firmware V4; anything else gets no name.
std::string accessLevelName(const std::string& type, const std::string& firmware, int64_t level) {
    auto startsWith = [&](const char* p) { return type.compare(0, std::char_traits<char>::length(p), p) == 0; };
    const size_t digit = firmware.find_first_of("0123456789");
    const int major = digit == std::string::npos ? 0 : std::atoi(firmware.c_str() + digit);
    if (startsWith("S71500.") || (startsWith("S71200.") && major >= 4)) {
        switch (level) {
            case 1: return "Full access (no protection)";
            case 2: return "Read access";
            case 3: return "HMI access";
            case 4: return "No access (complete protection)";
        }
    } else if (startsWith("S71200.") && major >= 1) {
        switch (level) {
            case 1: return "No protection";
            case 2: return "Write protection";
            case 3: return "Write/read protection";
        }
    }
    return std::string();
}

std::string projectEventText(const ProjectEvent& e) {
    if (e.event == "ProjectHistoryUserCreated") return "Project created with TIA Portal " + e.version;
    if (e.event == "ProjectHistoryConverted")
        return "Project converted to TIA Portal " + e.version +
               (e.oldVersion.empty() ? std::string() : " (from version " + e.oldVersion + ")");
    if (e.event == "ProjectSave") return "Project saved with TIA Portal " + e.version;
    return std::string();
}

Inventory buildInventory(const Project& project, const InventoryOptions& opt) {
    Inventory inv;
    const Container& c = project.container();
    const MetaModel& meta = project.meta();

    const uint32_t relParent = meta.relationId("BaseDeviceItemData", "Parent");
    const uint32_t relContainer = meta.relationId("BaseDeviceItemData", "Container");
    const uint32_t relNodeItem = meta.relationId("NodeData", "DeviceItem");
    const uint32_t relNodeSubnet = meta.relationId("NodeData", "Subnet");
    const uint32_t relDeviceParent = meta.relationId("BaseDeviceData", "ParentProject");
    if (!relParent || !relNodeItem)
        inv.warnings.push_back("the type model lacks the hardware relations; no devices can be listed");

    inv.stats.blocks = c.blocks().size();
    inv.stats.saves = c.saveCount();
    inv.stats.objectsAfterLastSave = c.objectsAfterLastSave();
    for (const auto& m : c.markers())
        if (m.kind == "commit") inv.saves.push_back(formatTicks(m.ticks));

    std::map<uint32_t, Role> roles;
    auto roleOf = [&](uint32_t type) {
        auto it = roles.find(type);
        if (it != roles.end()) return it->second;
        Role r = Role::None;
        if (const TypeDef* t = meta.findById(type)) {
            if (t->kind == TypeKind::ObjectType) {
                if (meta.derivesFrom(t->name, kBaseDevice)) r = Role::Device;
                else if (meta.derivesFrom(t->name, kBaseDeviceItem)) r = Role::Item;
                else if (meta.derivesFrom(t->name, kNode)) r = Role::Node;
                else if (meta.derivesFrom(t->name, kSubnet)) r = Role::Subnet;
                else if (meta.derivesFrom(t->name, kProject)) r = Role::Project;
                else if (meta.derivesFromShort(t->name, "ConnectionPointData")) r = Role::Connection;
                else if (meta.derivesFromShort(t->name, "MastersystemData")) r = Role::IoSystem;
                else if (meta.derivesFromShort(t->name, "DeviceFunctionRightSet")) r = Role::RightSet;
                else if (meta.derivesFromShort(t->name, "DeviceFunctionRight")) r = Role::Right;
                else if (meta.derivesFromShort(t->name, "CorePersistenceInfo")) r = Role::Persistence;
            }
        }
        roles[type] = r;
        return r;
    };

    std::map<Key, Device> devices;
    std::map<Key, ItemRec> items;
    std::map<Key, Subnet> subnets;
    // A connection end before its nodes are resolved to devices.
    struct ConnRec {
        Connection conn;
        Key self{0, 0};
        Key peer{0, 0};  // the other half, when the partner is in the project too
        bool hasPeer = false;
        std::vector<Key> localNodes, remoteNodes, remoteTargets;
    };
    struct IoRec {
        IoSystem sys;
        Key master{0, 0}, subnet{0, 0};
        bool hasMaster = false, hasSubnet = false;
        std::vector<Key> heads;
    };
    std::vector<IoRec> ioRecs;
    const uint32_t relIoMaster = meta.relationId("MastersystemData", "Master");
    const uint32_t relIoHeads = meta.relationId("MastersystemData", "HeadModules");
    const uint32_t relIoSubnet = meta.relationId("MastersystemData", "Subnet");
    const uint32_t relPortPeers = meta.relationId("DeviceItemBaseData", "PortToPorts");
    const uint32_t relPortInterface = meta.relationId("DeviceItemBaseData", "Interface");
    const uint32_t relConnPeer = meta.relationId("ConnectionPointData", "Conn2Conn");
    const uint32_t relRightSetOwner = meta.relationId("DeviceFunctionRightSet", "DeviceFunctionRightSetParent");
    const uint32_t relRightOwner = meta.relationId("DeviceFunctionRight", "DeviceFunctionRightParent");
    const uint32_t relEnvironment = meta.relationId("CoreObject", "Environment");
    const uint32_t relFolder = meta.relationId("FolderElementData", "AggregatingFolder");
    std::vector<ConnRec> conns;
    const uint32_t relConnNodes = meta.relationId("ConnectionPointData", "Conn2Nodes");
    const uint32_t relConnRemoteNodes = meta.relationId("ConnectionPointData", "Conn2RemoteNodes");
    const uint32_t relConnRemoteTargets = meta.relationId("ConnectionPointData", "Conn2RemoteTargets");

    struct NodeRec {
        Interface iface;
        Key self{0, 0};
        Key item{0, 0};
        bool hasItem = false;
        Key subnet{0, 0};
        bool hasSubnet = false;
    };
    std::vector<NodeRec> nodes;
    std::vector<std::pair<Key, ProjectInfo>> projects;
    // The set of user rights TIA Portal keeps for a CPU, by its name
    // ("...UmacFunctionRights_S71500V41"). Older CPUs have a small set too
    // (OPC UA users). What marks a CPU with user management is that the
    // access levels are among its rights.
    std::vector<std::pair<Key, std::string>> rightSets;
    std::vector<Key> accessByRights;

    for (const auto& kv : c.latest()) {
        const Block& b = c.blocks()[kv.second];
        if (c.isSystem(b)) continue;
        if (b.deleted()) {
            ++inv.stats.deletedObjects;
            continue;
        }
        ++inv.stats.liveObjects;
        Role role = roleOf(b.type);
        if (role == Role::None) continue;
        Object o;
        try {
            if (!project.decode(b, o)) continue;
        } catch (const ParseError& e) {
            ++inv.stats.objectsWithProblems;
            continue;
        }
        ++inv.stats.decodedObjects;
        if (!o.problems.empty()) ++inv.stats.objectsWithProblems;
        const Key key{b.type, b.id};
        const char* core = "ICoreAttributes";

        if (role == Role::Project) {
            ProjectInfo p;
            p.found = true;
            p.name = o.attrString(core, "Name");
            p.created = o.attrString(core, "CreationTime");
            p.modified = o.attrString(core, "ModifiedTime");
            p.author = o.attrString(core, "Author");
            p.lastModifiedBy = o.attrString(core, "LastModifiedBy");
            projects.emplace_back(key, std::move(p));
        } else if (role == Role::Device) {
            Device d;
            d.id = b.id;
            d.name = o.attrString(core, "Name");
            d.type = o.attrString(core, "Subtype");
            // A device is part of the project when its environment is the
            // project. Stations with a controller hang directly under it;
            // distributed IO sits in a folder ("Ungrouped devices") instead.
            Key parent;
            const bool direct = o.relationTarget(relDeviceParent, parent);
            if (direct || o.relationTarget(relFolder, parent))
                if (const TypeDef* pt = meta.findById(parent.first)) d.parentType = pt->shortName();
            Key env;
            if (relEnvironment && o.relationTarget(relEnvironment, env)) {
                const TypeDef* et = meta.findById(env.first);
                d.inProject = et && meta.derivesFrom(et->name, kProject);
            } else if (direct) {
                const TypeDef* pt = meta.findById(parent.first);
                d.inProject = pt && meta.derivesFrom(pt->name, kProject);
            }
            devices[key] = std::move(d);
        } else if (role == Role::Item) {
            ItemRec r;
            Module& m = r.module;
            m.id = b.id;
            m.name = o.attrString(core, "Name");
            m.type = o.attrString(core, "Subtype");
            m.author = o.attrString(core, "Author");
            m.modified = o.attrString(core, "ModifiedTime");
            m.typeName = o.attrString("IDeviceItemData", "InvariantTypeName");
            m.orderNumber = o.attrString("IDeviceItemData", "OrderNumber");
            m.firmware = o.attrString("IDeviceItemData", "FwVersion");
            m.position = intAttr(o, "IDeviceItemData", "PositionNumber", &m.hasPosition);
            m.itemType = intAttr(o, "IDeviceItemData", "DeviceItemType");
            r.controller = meta.derivesFrom(o.def->name, kControllerTarget);
            r.security = readSecurity(o);
            r.hasInterfaceItem = relPortInterface && o.relationTarget(relPortInterface, r.interfaceItem);
            if (relPortPeers)
                for (const auto& rel : o.relations)
                    if (rel.relation == relPortPeers && (rel.targetType || rel.targetId))
                        r.portPeers.emplace_back(rel.targetType, rel.targetId);
            r.hasParent = o.relationTarget(relParent, r.parent);
            r.hasContainer = o.relationTarget(relContainer, r.container);
            items[key] = std::move(r);
        } else if (role == Role::Subnet) {
            Subnet s;
            s.id = b.id;
            s.name = o.attrString(core, "Name");
            s.netType = intAttr(o, "ISubnetData", "NetType");
            subnets[key] = std::move(s);
        } else if (role == Role::RightSet) {
            Key owner;
            if (relRightSetOwner && o.relationTarget(relRightSetOwner, owner))
                rightSets.emplace_back(owner, o.attrString("IDeviceFunctionRightSetAttributes", "DeviceFunctionRightSetId"));
        } else if (role == Role::Right) {
            static const char kLevelRight[] = ".ProtectionLevelFullAccess";
            const std::string name = o.attrString(core, "Name");
            const size_t len = sizeof kLevelRight - 1;
            Key owner;
            if (name.size() >= len && name.compare(name.size() - len, len, kLevelRight) == 0 && relRightOwner &&
                o.relationTarget(relRightOwner, owner))
                accessByRights.push_back(owner);
        } else if (role == Role::IoSystem) {
            IoRec r;
            r.sys.id = b.id;
            r.sys.name = o.attrString(core, "Name");
            r.sys.kind = o.attrString("IConfigBaseData", "ConfigObjectTypeName");
            r.sys.hasNumber = readNumber(o, "PositionNumber", r.sys.number);
            r.hasMaster = relIoMaster && o.relationTarget(relIoMaster, r.master);
            r.hasSubnet = relIoSubnet && o.relationTarget(relIoSubnet, r.subnet);
            if (relIoHeads)
                for (const auto& rel : o.relations)
                    if (rel.relation == relIoHeads && (rel.targetType || rel.targetId))
                        r.heads.emplace_back(rel.targetType, rel.targetId);
            ioRecs.push_back(std::move(r));
        } else if (role == Role::Persistence) {
            // The history TIA Portal keeps itself, a list of structures.
            const Value* h = o.attr("IPersistenceInfoAttributes", "History");
            if (h && h->type == Value::Type::List)
                for (const Value& rec : h->elements) {
                    auto text = [&](const char* name) {
                        const Value* f = rec.field(name);
                        return f && (f->type == Value::Type::String || f->type == Value::Type::DateTime) ? f->s
                                                                                                         : std::string();
                    };
                    ProjectEvent e;
                    e.date = text("Date");
                    e.event = text("FeedbackId");
                    e.version = text("CurrentVersion");
                    e.oldVersion = text("OldVersion");
                    e.logFile = text("LogFile");
                    inv.events.push_back(std::move(e));
                }
        } else if (role == Role::Connection) {
            ConnRec r;
            r.self = key;
            r.hasPeer = relConnPeer && o.relationTarget(relConnPeer, r.peer);
            Connection& cn = r.conn;
            cn.id = b.id;
            cn.name = o.attrString(core, "Name");
            cn.kind = o.attrString("IConfigBaseData", "ConfigObjectTypeName");
            if (meta.derivesFromShort(o.def->name, "HmiConnectionPointData")) cn.kind = "HMI";
            else if (cn.kind == "S7ConnectionPoint") cn.kind = "S7";
            cn.hasLocalId = readNumber(o, "ConnId", cn.localId);
            readSetting(o, "ConnS7TcpIp", cn.overTcpIp);
            readSetting(o, "ConnOneWay", cn.oneWay);
            readSetting(o, "ConnEstablishment", cn.activeEstablishment);
            if (const Value* v = o.expandoValue("ConnRemoteAddress"))
                if (v->type == Value::Type::String) cn.partnerAddress = v->s;
            for (const auto& rel : o.relations) {
                const Key target{rel.targetType, rel.targetId};
                if (!rel.relation || (!target.first && !target.second)) continue;
                if (rel.relation == relConnNodes) r.localNodes.push_back(target);
                else if (rel.relation == relConnRemoteNodes) r.remoteNodes.push_back(target);
                else if (rel.relation == relConnRemoteTargets) r.remoteTargets.push_back(target);
            }
            conns.push_back(std::move(r));
        } else if (role == Role::Node) {
            NodeRec n;
            n.self = key;
            Interface& i = n.iface;
            i.id = b.id;
            i.name = o.attrString(core, "Name");
            i.nodeId = o.attrString("INodeData", "NodeID");
            i.nodeType = intAttr(o, "INodeData", "NodeType");
            if (o.expandoValue("NodeIPAddress")) {
                i.hasIpSettings = true;
                i.ip = expandoIp(o, "NodeIPAddress");
                i.mask = expandoIp(o, "NodeIPSubnetMask");
                const Value* routerUsed = o.expandoValue("NodeIPDefaultRouterAddressUsed");
                if (routerUsed && routerUsed->truthy()) i.router = expandoIp(o, "NodeIPDefaultRouterAddress");
                const Value* ipUsed = o.expandoValue("NodeIPProtocolUsed");
                i.ipProtocolUsed = ipUsed && ipUsed->truthy();
                uint64_t cfg = 0;
                if (const Value* v = o.expandoValue("NodeIPConfiguration"))
                    if (v->asUInt(cfg)) i.ipConfiguration = static_cast<int64_t>(cfg);
                i.ipAssignedElsewhere = cfg != 0;
                if (const Value* v = o.expandoValue("NodeIPAddresseSetByUser")) {
                    i.hasIpSetByUser = true;
                    i.ipSetByUser = v->truthy();
                }
            }
            if (const Value* v = o.expandoValue("NodePBMpiAddress")) {
                uint64_t u = 0;
                if (v->asUInt(u)) {
                    i.hasBusAddress = true;
                    i.busAddress = static_cast<int64_t>(u);
                }
            }
            if (const Value* v = o.expandoValue("PnNameOfStation"))
                if (v->type == Value::Type::String) i.profinetNameStored = v->s;
            if (const Value* v = o.expandoValue("PnPnNoSAutoGenerate")) i.profinetNameAuto = v->truthy();
            if (!i.profinetNameAuto) i.profinetName = i.profinetNameStored;
            if (const Value* v = o.expandoValue("NodeMacAddress")) {
                uint64_t u = 0;
                if (v->asUInt(u) && u) i.configuredMac = macAddress(u);
            }
            n.hasItem = o.relationTarget(relNodeItem, n.item);
            n.hasSubnet = o.relationTarget(relNodeSubnet, n.subnet);
            nodes.push_back(std::move(n));
        }
    }

    if (!projects.empty()) {
        inv.project = projects.front().second;  // lowest (type, id)
        if (projects.size() > 1)
            inv.warnings.push_back("several project objects found; reporting \"" + inv.project.name + "\"");
    } else {
        inv.warnings.push_back("no project object found");
    }

    // The device an item belongs to: follow Parent until a device is reached.
    auto owningDevice = [&](Key k, Key& out) {
        for (int depth = 0; depth < 64; ++depth) {
            auto it = items.find(k);
            if (it == items.end() || !it->second.hasParent) return false;
            k = it->second.parent;
            if (devices.count(k)) {
                out = k;
                return true;
            }
        }
        return false;
    };
    // The nearest enclosing item with an order number, starting at the item itself.
    auto owningModule = [&](Key k, Key& out) {
        for (int depth = 0; depth < 64; ++depth) {
            auto it = items.find(k);
            if (it == items.end()) return false;
            if (!it->second.module.orderNumber.empty()) {
                out = k;
                return true;
            }
            if (!it->second.hasParent) return false;
            k = it->second.parent;
        }
        return false;
    };

    // Attach interfaces to the module that carries them and to their subnet.
    for (auto& n : nodes) {
        if (n.hasSubnet) {
            auto s = subnets.find(n.subnet);
            if (s != subnets.end()) n.iface.subnet = s->second.name;
        }
        if (!n.hasItem || !items.count(n.item)) {
            ++inv.stats.unattachedItems;
            continue;
        }
        Key target = n.item;
        Key mod;
        if (items[n.item].module.orderNumber.empty() && owningModule(n.item, mod)) {
            n.iface.item = items[n.item].module.name;
            target = mod;
        }
        if (n.hasSubnet) {
            auto s = subnets.find(n.subnet);
            if (s != subnets.end()) {
                SubnetMember sm;
                Key dev;
                if (owningDevice(target, dev)) sm.device = devices[dev].name;
                sm.module = items[target].module.name;
                sm.node = n.iface.name;
                sm.ip = n.iface.ip;
                s->second.members.push_back(std::move(sm));
            }
        }
        items[target].module.interfaces.push_back(n.iface);
    }

    // Connections name their ends by node; say which device and module that is.
    std::map<Key, ConnectionEnd> nodeEnds;
    auto endOfModule = [&](Key mod) {
        ConnectionEnd e;
        auto it = items.find(mod);
        if (it == items.end()) return e;
        e.module = it->second.module.name;
        Key dev;
        if (owningDevice(mod, dev)) e.device = devices[dev].name;
        return e;
    };
    for (const auto& n : nodes) {
        if (!n.hasItem || !items.count(n.item)) continue;
        Key mod = n.item;
        if (items[n.item].module.orderNumber.empty()) owningModule(n.item, mod);
        ConnectionEnd e = endOfModule(mod);
        e.interface = n.iface.name;
        e.ip = n.iface.ip;
        nodeEnds[n.self] = std::move(e);
    }
    for (auto& r : conns) {
        Connection& cn = r.conn;
        for (const Key& k : r.localNodes) {
            auto e = nodeEnds.find(k);
            if (e != nodeEnds.end()) {
                cn.local = e->second;
                break;
            }
        }
        for (const Key& k : r.remoteNodes) {
            auto e = nodeEnds.find(k);
            if (e != nodeEnds.end()) {
                cn.partner = e->second;
                break;
            }
        }
        // The partner controller is named even when no interface of it is.
        if (cn.partner.module.empty())
            for (const Key& k : r.remoteTargets) {
                ConnectionEnd e = endOfModule(k);
                if (!e.module.empty()) {
                    cn.partner = e;
                    break;
                }
            }
    }
    // A connection between two devices of the project is stored as two
    // halves that point at each other. Report it once, from the side that
    // opens the connection (the lower object id when that does not decide).
    std::map<Key, size_t> connIndex;
    for (size_t i = 0; i < conns.size(); ++i) connIndex[conns[i].self] = i;
    std::vector<bool> dropped(conns.size(), false);
    for (size_t i = 0; i < conns.size(); ++i) {
        ConnRec& r = conns[i];
        if (!r.hasPeer || dropped[i]) continue;
        auto p = connIndex.find(r.peer);
        if (p == connIndex.end() || p->second == i) continue;
        ConnRec& other = conns[p->second];
        if (!other.hasPeer || other.peer != r.self || dropped[p->second]) continue;
        const bool mineOpens = r.conn.activeEstablishment.stored && r.conn.activeEstablishment.on;
        const bool theirsOpens = other.conn.activeEstablishment.stored && other.conn.activeEstablishment.on;
        const bool keepMine = mineOpens != theirsOpens ? mineOpens : r.self < other.self;
        ConnRec& keep = keepMine ? r : other;
        ConnRec& drop = keepMine ? other : r;
        keep.conn.partner = drop.conn.local;
        keep.conn.bothSides = true;
        keep.conn.hasPartnerId = drop.conn.hasLocalId;
        keep.conn.partnerId = drop.conn.localId;
        dropped[keepMine ? p->second : i] = true;
    }
    for (size_t i = 0; i < conns.size(); ++i) {
        Connection& cn = conns[i].conn;
        if (dropped[i]) continue;
        // Ends kept for library copies have no place in the project.
        if (cn.local.device.empty() && cn.partner.device.empty() && cn.partnerAddress.empty()) continue;
        inv.connections.push_back(std::move(cn));
    }
    std::stable_sort(inv.connections.begin(), inv.connections.end(), [](const Connection& a, const Connection& b) {
        if (a.local.device != b.local.device) return a.local.device < b.local.device;
        return a.id < b.id;
    });

    // The controller an item belongs to: the item itself or one above it.
    auto controllerOf = [&](Key k, Key& out) {
        for (int depth = 0; depth < 64; ++depth) {
            auto it = items.find(k);
            if (it == items.end()) return false;
            if (it->second.controller) {
                out = k;
                return true;
            }
            if (!it->second.hasParent) return false;
            k = it->second.parent;
        }
        return false;
    };
    // Interfaces by the item they sit on.
    std::map<Key, const Interface*> itemInterface;
    for (const auto& n : nodes)
        if (n.hasItem) itemInterface.emplace(n.item, &n.iface);

    // IO systems: the controller behind the "master" item, and the device
    // behind each head module.
    for (auto& r : ioRecs) {
        IoSystem& sys = r.sys;
        Key ctrl;
        if (r.hasMaster && controllerOf(r.master, ctrl)) {
            sys.controller = items[ctrl].module.name;
            Key dev;
            if (owningDevice(ctrl, dev)) sys.controllerDevice = devices[dev].name;
        }
        if (r.hasSubnet) {
            auto s = subnets.find(r.subnet);
            if (s != subnets.end()) sys.subnet = s->second.name;
        }
        for (const Key& head : r.heads) {
            auto h = items.find(head);
            if (h == items.end()) continue;
            IoDevice d;
            Key mod;
            if (owningModule(head, mod)) {
                d.module = items[mod].module.name;
                items[mod].module.ioController = sys.controller;
                items[mod].module.ioSystem = sys.name;
                Key dev;
                if (owningDevice(mod, dev)) d.device = devices[dev].name;
            }
            // the head module hangs on the interface it is reached through
            if (h->second.hasParent) {
                auto i = itemInterface.find(h->second.parent);
                if (i != itemInterface.end()) {
                    d.interface = i->second->name;
                    d.ip = i->second->ip;
                }
            }
            sys.devices.push_back(std::move(d));
        }
        // Library copies of a controller keep a copy of its IO system.
        Key dev;
        if (!r.hasMaster || !owningDevice(r.master, dev) || !devices[dev].inProject) continue;
        inv.ioSystems.push_back(std::move(sys));
    }
    std::stable_sort(inv.ioSystems.begin(), inv.ioSystems.end(),
                     [](const IoSystem& a, const IoSystem& b) { return a.id < b.id; });

    // Port to port cabling. Each port names the other; keep one of the two.
    auto portEnd = [&](Key port) {
        PortEnd e;
        e.id = port.second;
        auto it = items.find(port);
        if (it == items.end()) return e;
        const Module& pm = it->second.module;
        e.port = pm.typeName.empty() ? pm.name : pm.typeName;
        // A port can carry the order number of a plug-in adapter; the module
        // it belongs to is found through its interface.
        Key from = it->second.hasInterfaceItem ? it->second.interfaceItem
                                               : (it->second.hasParent ? it->second.parent : port);
        Key mod;
        if (owningModule(from, mod)) {
            e.module = items[mod].module.name;
            Key dev;
            if (owningDevice(mod, dev)) e.device = devices[dev].name;
        }
        return e;
    };
    for (const auto& kv : items)
        for (const Key& peer : kv.second.portPeers) {
            auto other = items.find(peer);
            if (other == items.end()) continue;
            const auto& back = other->second.portPeers;
            const bool mutual = std::find(back.begin(), back.end(), kv.first) != back.end();
            if (mutual && !(kv.first < peer)) continue;
            Key dev;
            if (!owningDevice(kv.first, dev) || !devices[dev].inProject) continue;
            PortLink link;
            link.a = portEnd(kv.first);
            link.b = portEnd(peer);
            inv.portLinks.push_back(std::move(link));
        }

    // Settings sit on the controller and on the items below it; bring them
    // together on the controller.
    for (auto& kv : items) {
        const ItemSecurity& from = kv.second.security;
        Key k = kv.first;
        ItemRec* ctrl = nullptr;
        for (int depth = 0; depth < 64 && !ctrl; ++depth) {
            auto it = items.find(k);
            if (it == items.end()) break;
            if (it->second.controller) ctrl = &it->second;
            else if (!it->second.hasParent) break;
            else k = it->second.parent;
        }
        if (!ctrl) continue;
        Security& s = ctrl->module.security;
        const Security& f = from.s;
        if (f.hasAccessLevel) {
            s.hasAccessLevel = true;
            s.accessLevel = f.accessLevel;
        }
        take(s.putGet, f.putGet);
        take(s.webServer, f.webServer);
        take(s.webServerHttpsOnly, f.webServerHttpsOnly);
        take(s.opcUaServer, f.opcUaServer);
        take(s.displayProtection, f.displayProtection);
        take(s.accessControl, f.accessControl);
        take(s.accessControlViaAccessLevels, f.accessControlViaAccessLevels);
        take(s.configDataProtection, f.configDataProtection);
        if (f.hasCommunicationMode) {
            s.hasCommunicationMode = true;
            s.communicationMode = f.communicationMode;
        }
        if (f.hasTimeSyncRole) {
            s.hasTimeSyncRole = true;
            s.timeSyncRole = f.timeSyncRole;
            if (f.timeSyncRole == 2)
                s.ntpServers.insert(s.ntpServers.end(), f.ntpServers.begin(), f.ntpServers.end());
        }
        if (from.webAccess.stored && from.webAccess.on) {
            const Module& im = kv.second.module;
            s.webServerInterfaces.push_back(im.typeName.empty() ? im.name : im.typeName);
        }
    }
    for (const auto& rs : rightSets) {
        auto it = items.find(rs.first);
        if (it != items.end() && it->second.controller) it->second.module.security.functionRightSet = rs.second;
    }
    for (const Key& k : accessByRights) {
        auto it = items.find(k);
        if (it != items.end() && it->second.controller) it->second.module.security.userManagement = true;
    }
    for (auto& kv : items) {
        Module& m = kv.second.module;
        if (kv.second.controller && m.security.hasAccessLevel)
            m.security.accessLevelName = accessLevelName(m.type, m.firmware, m.security.accessLevel);
        std::sort(m.security.webServerInterfaces.begin(), m.security.webServerInterfaces.end());
    }

    for (auto& kv : items) {
        ItemRec& r = kv.second;
        Module& m = r.module;
        Key dev;
        if (!owningDevice(kv.first, dev)) {
            ++inv.stats.unattachedItems;
            continue;
        }
        const bool port = (m.itemType & kItemPort) != 0;
        if (r.controller) m.kind = "controller";
        else if (port) m.kind = "port";
        else if (m.itemType & kItemRack) m.kind = "rack";
        else if (m.itemType & kItemModule) m.kind = "module";
        else if (!m.interfaces.empty() && m.orderNumber.empty()) m.kind = "interface";
        else if (m.itemType & kItemSubmodule) m.kind = "submodule";
        else m.kind = "item";
        if (r.hasContainer) {
            auto ci = items.find(r.container);
            if (ci != items.end()) m.container = ci->second.module.name;
        }
        if (!opt.allItems) {
            // Newer CPUs carry an internal "virtual" interface without an
            // address and a pseudo module for it; neither is a real port.
            m.interfaces.erase(std::remove_if(m.interfaces.begin(), m.interfaces.end(),
                                              [](const Interface& i) {
                                                  return i.hasIpSettings && i.ip == "0.0.0.0" && i.mask == "0.0.0.0" &&
                                                         !i.ipAssignedElsewhere && i.subnet.empty() && !i.hasBusAddress;
                                              }),
                               m.interfaces.end());
        }
        const bool pseudo = m.orderNumber.compare(0, 8, "Virtual ") == 0;
        const bool listed =
            opt.allItems || (!m.orderNumber.empty() && !port && !pseudo) || !m.interfaces.empty();
        if (listed) devices[dev].modules.push_back(m);
    }

    for (auto& kv : devices) {
        Device& d = kv.second;
        std::stable_sort(d.modules.begin(), d.modules.end(), [](const Module& a, const Module& b) {
            if (a.position != b.position) return a.position < b.position;
            return a.id < b.id;
        });
        inv.devices.push_back(std::move(d));
    }
    std::stable_sort(inv.devices.begin(), inv.devices.end(), [](const Device& a, const Device& b) {
        if (a.inProject != b.inProject) return a.inProject;
        return a.id < b.id;
    });
    for (auto& kv : subnets) inv.subnets.push_back(std::move(kv.second));

    if (!c.complete()) inv.warnings.push_back("the block list ended early: " + c.stopReason());
    if (c.hashesVerified() && c.hashErrors())
        inv.warnings.push_back(std::to_string(c.hashErrors()) + " block(s) failed the SHA-256 check");
    if (inv.stats.objectsWithProblems)
        inv.warnings.push_back(std::to_string(inv.stats.objectsWithProblems) +
                               " hardware object(s) were only partly decoded");
    return inv;
}

}  // namespace tia
