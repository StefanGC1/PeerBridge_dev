#include "p2p_system.hpp"
#include "logger.hpp"
#include "stun.hpp"
#include "ipc.hpp"
#include <iostream>
#include <vector>
#include <sstream>

namespace {
// REMOVE LATER
inline void dumpMinecraftLan(const std::vector<uint8_t>& buf,
                             const std::string& textPrefix) {
    if (buf.size() < 34) return;                  // IPv4 + UDP header minimal
    const uint8_t  ihl   = (buf[0] & 0x0F) * 4;   // usually 20
    const uint16_t dport = (buf[ihl+2] << 8) | buf[ihl+3];

    // Filter: UDP 4445 or 4446 AND dest 224.0.2.60
    // if (dport != 4445 && dport != 4446) return;
    // if (buf[16] != 224 || buf[17] != 0 || buf[18] != 2 || buf[19] != 60) return;

    std::ostringstream out;
    out << textPrefix << " MC-LAN • size " << buf.size() - ihl - 8 << " B  | ";

    // payload starts after UDP header (ihl + 8)
    for (size_t i = ihl + 8; i < buf.size(); ++i) {
        uint8_t c = buf[i];
        out << (std::isprint(c) ? static_cast<char>(c) : '.');
    }
    clog << out.str() << std::endl;
}
}

// Helper structure for IP packet parsing
struct IPPacket {
    uint8_t version_ihl;
    uint8_t dscp_ecn;
    uint16_t total_length;
    uint16_t identification;
    uint16_t flags_fragment_offset;
    uint8_t ttl;
    uint8_t protocol;
    uint16_t header_checksum;
    uint32_t source_ip;
    uint32_t dest_ip;
    // Options and data follow
};

P2PSystem::P2PSystem() 
    : running_(false), public_port_(0), peer_port_(0), is_host_(false)
{
}

P2PSystem::~P2PSystem() {
    disconnect();
}

bool P2PSystem::initialize(const std::string& server_url, const std::string& username, int local_port) {
    username_ = username;
    running_ = true;
    
    // Set up callbacks for signaling - REMOVED
    // signaling_.setConnectCallback([this](bool connected) {
    //     if (connected) {
    //         this->signaling_.sendGreeting();
    //     }
    // });
    // 
    // signaling_.setChatRequestCallback([this](const std::string& from) {
    //     this->handleConnectionRequest(from);
    // });
    // 
    // signaling_.setPeerInfoCallback([this](const std::string& username, const std::string& ip, int port) {
    //     this->handlePeerInfo(username, ip, port);
    // });
    // 
    // signaling_.setChatInitCallback([this](const std::string& username, const std::string& ip, int port) {
    //     this->handleConnectionInit(username, ip, port);
    // });

    // Start IPC server
    std::string ipc_server_address = "0.0.0.0:50051";
    if (!startIPCServer(ipc_server_address)) {
        std::cerr << "Failed to start IPC server. Exiting." << std::endl;
        return 1;
    }
    clog << "IPC Server started on " << ipc_server_address << std::endl;

    // Discover public address for NAT traversal
    if (!discoverPublicAddress()) {
        if (on_status_) {
            on_status_("Failed to discover public address, NAT traversal may not work");
        }
        return false;
    }

    // Create networking class, using the socket from STUN to preserve NAT binding
    network_ = std::make_unique<UDPNetwork>(
        std::move(stun_.getSocket()),
        stun_.getContext());
    
    // Set up network callbacks for P2P connection
    network_->setMessageCallback([this](const std::string& message) {
        // Convert message to binary data
        std::vector<uint8_t> data(message.begin(), message.end());
        this->handleNetworkData(data);
    });
    
    network_->setConnectionCallback([this](bool connected) {
        this->handleConnectionChange(connected);
    });
    
    // Initialize TUN interface
    tun_ = std::make_unique<TunInterface>();
    if (!tun_->initialize("PeerBridge")) {
        if (on_status_) {
            on_status_("Failed to initialize TUN interface");
        }
        return false;
    }
    
    // Register packet callback from TUN interface
    tun_->setPacketCallback([this](const std::vector<uint8_t>& packet) {
        this->handlePacketFromTun(packet);
    });
    
    // Connect to signaling server - REMOVED
    // if (!signaling_.connect(server_url)) {
    //     if (on_status_) {
    //         on_status_("Failed to connect to signaling server");
    //     }
    //     return false;
    // }
    
    // Start UDP network
    if (!network_->startListening(local_port)) {
        if (on_status_) {
            on_status_("Failed to start UDP network");
        }
        return false;
    }

    // Register with the signaling server - REMOVED
    // signaling_.registerUser(username_, public_ip_, public_port_);

    if (on_status_) {
        on_status_("P2P System initialized successfully");
    }
    
    return true;
}

bool P2PSystem::discoverPublicAddress() {
    auto public_addr = stun_.discoverPublicAddress();
    if (!public_addr) {
        return false;
    }
    
    public_ip_ = public_addr->ip;
    public_port_ = public_addr->port;
    
    if (on_status_) {
        on_status_("Public address: " + public_ip_ + ":" + std::to_string(public_port_));
    }
    
    return true;
}

bool P2PSystem::connectToPeer(const std::string& peer_username) {
    if (network_->isConnected()) {
        if (on_status_) {
            on_status_("Already connected to a peer");
        }
        return false;
    }
    
    peer_username_ = peer_username;
    is_host_ = false;
    
    // Request peer info from signaling server
    // signaling_.requestPeerInfo(peer_username);
    
    // Request connection
    // signaling_.sendChatRequest(peer_username);
    
    if (on_status_) {
        on_status_("Sent connection request to " + peer_username);
    }
    
    return true;
}

void P2PSystem::disconnect() {
    running_ = false;
    
    // Stop packet handling
    if (packet_handling_thread_.joinable()) {
        packet_handling_thread_.join();
    }
    
    // Stop the network interface
    stopNetworkInterface();
    
    // Disconnect from peer
    network_->disconnect();
    // signaling_.disconnect(); // REMOVED
    
    peer_username_ = "";
    pending_request_from_ = "";
    
    // Stop IPC server
    stopIPCServer();
    
    if (on_status_) {
        on_status_("Disconnected");
    }
}

bool P2PSystem::isConnected() const {
    return network_->isConnected();
}

bool P2PSystem::isRunning() const {
    return running_;
}

bool P2PSystem::isHost() const {
    return is_host_;
}

void P2PSystem::acceptIncomingRequest() {
    if (pending_request_from_.empty()) {
        if (on_status_) {
            on_status_("No pending connection request");
        }
        return;
    }
    
    // We are the host
    is_host_ = true;
    
    // signaling_.acceptChatRequest();
    if (on_status_) {
        on_status_("Accepted connection request from " + pending_request_from_);
    }
    
    peer_username_ = pending_request_from_;
    pending_request_from_ = "";
}

void P2PSystem::rejectIncomingRequest() {
    if (pending_request_from_.empty()) {
        if (on_status_) {
            on_status_("No pending connection request");
        }
        return;
    }
    
    // signaling_.declineChatRequest();
    if (on_status_) {
        on_status_("Rejected connection request from " + pending_request_from_);
    }
    
    pending_request_from_ = "";
}

bool P2PSystem::startNetworkInterface() {
    if (!network_->isConnected()) {
        if (on_status_) {
            on_status_("Not connected to a peer");
        }
        return false;
    }
    
    // Assign IP addresses based on host/client role
    assignIPAddresses();
    
    // Set up virtual interface
    if (!setupVirtualInterface()) {
        if (on_status_) {
            on_status_("Failed to set up virtual interface");
        }
        return false;
    }
    
    // Start packet processing
    if (!tun_->startPacketProcessing()) {
        if (on_status_) {
            on_status_("Failed to start packet processing");
        }
        return false;
    }
    
    if (on_status_) {
        on_status_("Network interface started with IP " + local_virtual_ip_);
        on_status_("Peer has IP " + peer_virtual_ip_);
    }

    clog << "Connection successful! Turning off logging." << std::endl;
    clog.setLoggingEnabled(false);
    std::flush(std::cout);
    std::flush(std::cerr);
    
    return true;
}

void P2PSystem::stopNetworkInterface() {
    if (tun_ && tun_->isRunning()) {
        tun_->stopPacketProcessing();
        tun_->close();
        
        if (on_status_) {
            on_status_("Network interface stopped");
        }
    }
}

void P2PSystem::assignIPAddresses() {
    if (is_host_) {
        local_virtual_ip_ = HOST_IP;
        peer_virtual_ip_ = CLIENT_IP;
    } else {
        local_virtual_ip_ = CLIENT_IP;
        peer_virtual_ip_ = HOST_IP;
    }
}

bool P2PSystem::setupVirtualInterface() {
    // Configure the interface with IP and netmask
    if (!tun_->configureInterface(local_virtual_ip_, VIRTUAL_NETMASK)) {
        if (on_status_) {
            on_status_("Failed to configure interface with IP " + local_virtual_ip_);
        }
        return false;
    }
    
    // Set up routing
    if (!tun_->setupRouting()) {
        if (on_status_) {
            on_status_("Failed to set up routing");
        }
        return false;
    }
    
    // Add firewall rule to allow traffic on the virtual network
    std::ostringstream firewallCmd;
    firewallCmd << "netsh advfirewall firewall add rule "
                << "name=\"P2P Network\" "
                << "dir=in "
                << "action=allow "
                << "protocol=any "
                << "remoteip=10.0.0.0/24";
    
    clog << "Adding firewall rule: " << firewallCmd.str() << std::endl;
    
    // Execute firewall command (using our TUN interface's command helper)
    if (!tun_->executeNetshCommand(firewallCmd.str())) {
        clog << "Warning: Failed to add inbound firewall rule. Connectivity may be limited." << std::endl;
    }
    
    // Add outbound rule too
    firewallCmd.str("");
    firewallCmd << "netsh advfirewall firewall add rule "
                << "name=\"P2P Network (out)\" "
                << "dir=out "
                << "action=allow "
                << "protocol=any "
                << "remoteip=10.0.0.0/24";
    
    if (!tun_->executeNetshCommand(firewallCmd.str())) {
        clog << "Warning: Failed to add outbound firewall rule. Connectivity may be limited." << std::endl;
    }
    
    // Add specific rule for ICMP (ping)
    firewallCmd.str("");
    firewallCmd << "netsh advfirewall firewall add rule "
                << "name=\"P2P Network ICMP\" "
                << "dir=in "
                << "action=allow "
                << "protocol=icmpv4 "
                << "remoteip=10.0.0.0/24";
    
    if (!tun_->executeNetshCommand(firewallCmd.str())) {
        clog << "Warning: Failed to add ICMP firewall rule. Ping may not work." << std::endl;
    }
    
    // Enable File and Printer Sharing (needed for some network discovery protocols)
    firewallCmd.str("");
    firewallCmd << "netsh advfirewall firewall set rule "
                << "group=\"File and Printer Sharing\" "
                << "new enable=Yes";
    
    if (!tun_->executeNetshCommand(firewallCmd.str())) {
        clog << "Warning: Failed to enable File and Printer Sharing. Network discovery may be limited." << std::endl;
    }

    // Add specific rule for IGMP (inbound)
    firewallCmd.str("");
    firewallCmd << "netsh advfirewall firewall add rule "
                << "name=\"P2P Network IGMP in\" "
                << "dir=in "
                << "action=allow "
                << "protocol=2 "
                << "remoteip=10.0.0.0/24";
    if (!tun_->executeNetshCommand(firewallCmd.str())) {
        clog << "Warning: Failed to add outbound IGMP firewall rule. Multicast may not work." << std::endl;
    }

    firewallCmd.str("");
    firewallCmd << "netsh advfirewall firewall add rule "
                << "name=\"P2P Network IGMP out\" "
                << "dir=out "
                << "action=allow "
                << "protocol=2 "
                << "remoteip=10.0.0.0/24";
    if (!tun_->executeNetshCommand(firewallCmd.str())) {
        clog << "Warning: Failed to add outbound IGMP firewall rule. Multicast may not work." << std::endl;
    }

    std::ostringstream netCategoryCmd;
    netCategoryCmd << "powershell -Command \"Set-NetConnectionProfile -InterfaceAlias 'P2PBridge' -NetworkCategory Private\"";

    if (!tun_->executeNetshCommand(netCategoryCmd.str())) {
        clog << "Warning: Failed to set network category to Private" << std::endl;
    }
    
    return true;
}

void P2PSystem::handleConnectionRequest(const std::string& from) {
    pending_request_from_ = from;
    
    // signaling_.handleConnectionRequest(from);
}

void P2PSystem::handlePeerInfo(const std::string& username, const std::string& ip, int port) {
    if (username != peer_username_) {
        return; // Not the peer we're looking for
    }
    
    peer_ip_ = ip;
    peer_port_ = port;
    
    if (on_status_) {
        on_status_("Got peer info: " + username + " at " + ip + ":" + std::to_string(port));
    }
}

void P2PSystem::handleConnectionInit(const std::string& username, const std::string& ip, int port) {
    peer_username_ = username;
    peer_ip_ = ip;
    peer_port_ = port;
    
    if (on_status_) {
        on_status_("Connection initialized with " + username + ", connecting...");
    }
    
    // Start UDP hole punching process
    if (!network_->connectToPeer(ip, port)) {
        if (on_status_) {
            on_status_("Failed to initiate UDP hole punching, will retry...");
        }
    }
}

void P2PSystem::handleNetworkData(const std::vector<uint8_t>& data) {
    // Debug: Log packet received from peer
    if (data.size() >= 20) { // Minimum IPv4 header size
        uint8_t protocol = data[9];
        uint32_t srcIp = (data[12] << 24) | (data[13] << 16) | (data[14] << 8) | data[15];
        uint32_t dstIp = (data[16] << 24) | (data[17] << 16) | (data[18] << 8) | data[19];
        
        clog << "NET Recv: " << uint32ToIp(srcIp) << " -> " << uint32ToIp(dstIp) 
                  << " (Proto: " << static_cast<int>(protocol) << ", Size: " << data.size() << ")" << std::endl;
    }
    
    // Process received data from the peer and deliver to TUN interface
    deliverPacketToTun(data);
}

void P2PSystem::handlePacketFromTun(const std::vector<uint8_t>& packet) {
    // We received a packet from our TUN interface, forward to peer
    if (packet.size() >= sizeof(IPPacket) && network_->isConnected()) {
        // Debug: Log packet from TUN interface
        if (packet.size() >= 20) { // Minimum IPv4 header size
            uint8_t protocol = packet[9];
            uint32_t srcIp = (packet[12] << 24) | (packet[13] << 16) | (packet[14] << 8) | packet[15];
            uint32_t dstIp = (packet[16] << 24) | (packet[17] << 16) | (packet[18] << 8) | packet[19];
            
            clog << "P2P Forward: " << uint32ToIp(srcIp) << " -> " << uint32ToIp(dstIp) 
                      << " (Proto: " << static_cast<int>(protocol) << ", Size: " << packet.size() << ")" << std::endl;
        }
        
        forwardPacketToPeer(packet);
    }
}

bool P2PSystem::forwardPacketToPeer(const std::vector<uint8_t>& packet) {
    // Check if the packet is an IPv4 packet
    if (packet.empty() || (packet[0] >> 4) != 4) {
        return false;  // Not an IPv4 packet
    }
    
    // Extract source and destination IPs for filtering
    uint32_t srcIp = (packet[12] << 24) | (packet[13] << 16) | (packet[14] << 8) | packet[15];
    uint32_t dstIp = (packet[16] << 24) | (packet[17] << 16) | (packet[18] << 8) | packet[19];
    
    std::string srcIpStr = uint32ToIp(srcIp);
    std::string dstIpStr = uint32ToIp(dstIp);
    
    // Forward packets that are meant for the peer OR are broadcast/multicast packets
    // Modified to allow multicast/broadcast packets necessary for game discovery
    bool isForPeer = (dstIpStr == peer_virtual_ip_);
    bool isBroadcast = (dstIpStr == "10.0.0.255" || dstIpStr == "255.255.255.255");
    bool isMulticast = (dstIp >> 28) == 14; // 224.0.0.0/4 (first octet 224-239)
    
    if (!isForPeer && !isBroadcast && !isMulticast) {
        clog << "Dropping packet not meant for peer: " << srcIpStr << " -> " << dstIpStr << std::endl;
        return false;
    }

    if (isMulticast) dumpMinecraftLan(packet, "[TX] Sending");
    
    // Convert packet to string for UDP transmission
    std::string packet_str(packet.begin(), packet.end());
    
    // Send the packet to the peer
    clog << "NET Send: " << srcIpStr << " -> " << dstIpStr 
              << " (Size: " << packet.size() << ")" << std::endl;
    return network_->sendMessage(packet_str);
}

bool P2PSystem::deliverPacketToTun(const std::vector<uint8_t>& packet) {
    // Check if the packet is an IPv4 packet
    if (packet.empty() || (packet[0] >> 4) != 4) {
        return false;  // Not an IPv4 packet
    }
    
    // Extract source and destination IPs for filtering
    uint32_t srcIp = (packet[12] << 24) | (packet[13] << 16) | (packet[14] << 8) | packet[15];
    uint32_t dstIp = (packet[16] << 24) | (packet[17] << 16) | (packet[18] << 8) | packet[19];
    
    std::string srcIpStr = uint32ToIp(srcIp);
    std::string dstIpStr = uint32ToIp(dstIp);
    
    // Only deliver packets that are meant for us, are broadcast packets, or multicast packets
    bool isForUs = (dstIpStr == local_virtual_ip_);
    bool isBroadcast = (dstIpStr == "10.0.0.255" || dstIpStr == "255.255.255.255");
    bool isMulticast = (dstIp >> 28) == 14; // 224.0.0.0/4 (first octet 224-239)
    
    if (!isForUs && !isBroadcast && !isMulticast) {
        clog << "Dropping received packet not meant for us: " << srcIpStr << " -> " << dstIpStr << std::endl;
        return false;
    }

    if (isMulticast) dumpMinecraftLan(packet, "[TX] Receiving");
    
    // Send the packet to the TUN interface
    clog << "Delivering packet to TUN: " << srcIpStr << " -> " << dstIpStr 
              << " (Size: " << packet.size() << ")" << std::endl;
    return tun_->sendPacket(packet);
}

std::string P2PSystem::uint32ToIp(uint32_t ipAddress) {
    std::ostringstream result;
    
    for (int i = 0; i < 4; i++) {
        uint8_t octet = (ipAddress >> (8 * (3 - i))) & 0xFF;
        result << static_cast<int>(octet);
        if (i < 3) {
            result << ".";
        }
    }
    
    return result.str();
}

void P2PSystem::handleConnectionChange(bool connected) {
    if (on_connection_) {
        on_connection_(connected, peer_username_);
    }
    
    if (connected) {
        if (on_status_) {
            on_status_("Connected to " + peer_username_);
        }
        
        // Start the virtual network interface now that we're connected
        if (!startNetworkInterface()) {
            if (on_status_) {
                on_status_("Failed to start network interface, connection will be limited");
            }
        }
    } else {
        if (on_status_) {
            on_status_("Disconnected from peer");
        }
        
        // Stop the network interface
        stopNetworkInterface();
        
        peer_username_ = "";
    }
}

void P2PSystem::setStatusCallback(StatusCallback callback) {
    on_status_ = std::move(callback);
}

void P2PSystem::setConnectionCallback(ConnectionCallback callback) {
    on_connection_ = std::move(callback);
}

void P2PSystem::setConnectionRequestCallback(ConnectionRequestCallback callback) {
    on_connection_request_ = std::move(callback);
}

// Add getters for public IP and port
std::string P2PSystem::getPublicIP() const {
    return public_ip_;
}

int P2PSystem::getPublicPort() const {
    return public_port_;
}

bool P2PSystem::startConnection(const std::vector<std::string>& peer_info, int self_index) {
    if (on_status_) {
        on_status_("Starting connection with peers...");
    }
    
    // For now, just log the peer info
    for (size_t i = 0; i < peer_info.size(); i++) {
        if (i == self_index) {
            if (on_status_) {
                on_status_(std::string("Peer ") + std::to_string(i) + ": self");
            }
            continue;
        }
        
        if (peer_info[i] == "unavailable") {
            if (on_status_) {
                on_status_(std::string("Peer ") + std::to_string(i) + ": unavailable");
            }
            continue;
        }
        
        if (on_status_) {
            on_status_(std::string("Peer ") + std::to_string(i) + ": " + peer_info[i]);
        }
    }
    
    // TODO: Implement actual connection logic here
    // For testing, we'll just return success
    return true;
}

bool P2PSystem::startIPCServer(const std::string& server_address) {
    // Create IPC server if it doesn't exist
    if (!ipc_server_) {
        ipc_server_ = std::make_unique<IPCServer>();
        
        // Set up callbacks
        ipc_server_->setGetStunInfoCallback([this]() -> std::pair<std::string, int> {
            return {this->public_ip_, this->public_port_};
        });
        
        // Add shutdown callback
        ipc_server_->setShutdownCallback([this](bool force) {
            // Initiate process shutdown
            if (force) {
                // Force immediate exit
                std::cout << "Force shutdown requested, exiting immediately" << std::endl;
                exit(0);
            } else {
                // Graceful shutdown
                std::cout << "Graceful shutdown requested" << std::endl;
                
                // Refactorize this
                this->on_status_("Disconnected");
                // Signal the main loop to exit
                running_ = false;
            }
        });
        
        // Add start connection callback
        ipc_server_->setStartConnectionCallback([this](const std::vector<std::string>& peer_info, int self_index) -> bool {
            return this->startConnection(peer_info, self_index);
        });
    }
    
    // Run the server in a separate thread
    ipc_thread_ = std::thread([this, server_address]() {
        ipc_server_->RunServer(server_address);
    });
    
    return true;
}

void P2PSystem::stopIPCServer() {
    if (ipc_server_) {
        ipc_server_->ShutdownServer();
        
        if (ipc_thread_.joinable()) {
            ipc_thread_.join();
        }
    }
} 