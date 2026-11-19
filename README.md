# Reliable file transfer over UDP (C++)

Semester project for the KDS course at the Czech Technical University in Prague.

UDP has no delivery guarantees, so this console app builds reliability on top of it so that files and messages can be sent between two computers:

- **Packets of 1024 bytes**, each with a type, an index, the total packet count and a **CRC-32** checksum.
- **Sliding window** with **ACK/NACK** and timeouts. Packets that are lost or corrupted get resent automatically.
- **SHA-256 hash** of the whole file, checked on the receiving side to confirm the file arrived intact.
- **Multi-threaded** send and receive using `std::thread`, mutexes and condition variables.

**Building:** open `UDP_Communication_Framework.sln` in Visual Studio (Windows, Winsock) and build it.

**Running:** start the app on both computers and enter the other computer's IP address and ports.
- Type a message and press Enter to send it.
- `-f <path>` sends a file.
- `-e` exits.
