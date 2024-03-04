#include "network_server_manager.h"

#include <mutex>
#include <queue>
#include <thread>
#include <utility>

#include "logger.h"
#include "network_server_manager.h"

NetworkServerManager::~NetworkServerManager() {
  running = false;
  listener.close();
}

bool NetworkServerManager::Bind(unsigned short port) {
  return listener.listen(port) == sf::Socket::Done;
}

void NetworkServerManager::ListenToClientPackets(
    std::function<bool(sf::TcpSocket*, const PacketType, sf::Packet)>
        onMessageReceived) {
  this->onClientMessageReceived = std::move(onMessageReceived);
}

void NetworkServerManager::StartThreads() {
  std::thread clientAcceptor = std::thread([this]() {
    while (running) {
      // Accept a new connection
      std::scoped_lock lock(mutex);
      auto* socket = new sf::TcpSocket();

      if (listener.accept(*socket) != sf::Socket::Done) {
        LOG_ERROR("Could not accept connection");
      } else {
        auto index = clients.AddClient(socket);
        auto& client = *clients[index];

        LOG("Client connected: " << client.socket->getRemoteAddress() << ':'
                                 << client.socket->getRemotePort());

        // Start a new thread to receive packets from the client
        std::thread clientReceiver =
            std::thread([this, index]() { ReceivePacketFromClient(index); });
        clientReceiver.detach();
      }
    }
  });
  clientAcceptor.detach();

  std::thread packetSender = std::thread([&] {
    while (running) {
      if (clients.isEmpty()) continue;

      clients.CheckPacketToBeSent();
    }
  });
  packetSender.detach();
}

void NetworkServerManager::SendMessageToAllClients(const MessagePacket& message,
                                                   sf::TcpSocket* sender) {
  clients.SendPacketToAllClients(PacketManager::CreatePacket(message), sender);
}

void NetworkServerManager::SendHasPlayedToOneClient(
    const const HasPlayedPacket& packet, sf::TcpSocket* client) {
  clients.SendPacketToOneClient(PacketManager::CreatePacket(packet), client);
}

void NetworkServerManager::StartGame(const StartGamePacket& packet) {
  clients.SendPacketToAllClients(PacketManager::CreatePacket(packet));
}

void NetworkServerManager::ReceivePacketFromClient(std::size_t clientIndex) {
  bool receiving = true;
  auto* client = clients[clientIndex];
  sf::TcpSocket* socket = client->socket;
  while (receiving) {
    // Receive a message from the client
    sf::Packet answer;
    PacketType packetType = PacketManager::ReceivePacket(*socket, answer);

    if (packetType == PacketType::Invalid) {
      LOG_ERROR("Could not receive packet from client");
      break;
    }

    if (packetType == PacketType::Acknowledgement) {
      clients.Acknowledge(clientIndex);
      continue;
    }

    if (onClientMessageReceived) {
      receiving = onClientMessageReceived(socket, packetType, answer);
      LOG("recieve (" << receiving << ") packet " << (int)packetType
                      << " from (" << socket->getRemotePort() << ")");

      if (receiving) {
        client->packetsToBeSent.push(
            PacketManager::CreatePacket(AcknowledgementPacket()));
      }
    }
  }

  clients.RemoveClient(clientIndex);
}