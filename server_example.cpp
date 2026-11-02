#include "NCL.hpp"
#include <iostream>

int main() {
    try {
        // 1. Open a TCP server on this computer, port 5000.
        ncl::ServerSocket server(5000, "127.0.0.1");
        std::cout << "Waiting for a client..." << std::endl;

        // 2. Wait for a client to connect.
        ncl::Socket client = server.accept();

        // 3. Read one message and send a reply.
        auto message = client.receiveMessage();
        if (message) {
            std::cout << "Client: " << *message << '\n';
            client.sendMessage("Hello from the server!");
        }
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
}
