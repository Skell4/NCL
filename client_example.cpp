#include "NCL.hpp"
#include <iostream>

int main() {
    try {
        // 1. Connect to the server on this computer, port 5000.
        ncl::Socket socket("127.0.0.1", 5000);

        // 2. Send one message.
        socket.sendMessage("Hello from the client!");

        // 3. Read the server's reply.
        auto reply = socket.receiveMessage();
        if (reply) {
            std::cout << "Server: " << *reply << '\n';
        }
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
}
