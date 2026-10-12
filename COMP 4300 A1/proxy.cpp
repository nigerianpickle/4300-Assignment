#include <iostream>      // std::cout
#include <string>        // std::string
#include <cstdio>        // perror
#include <sys/socket.h>  // socket, bind, listen, accept, recv
#include <netinet/in.h>  // sockaddr_in, htons
#include <sstream>       // std::stringstream
#include <algorithm>    // transform
#include <netdb.h> 
#include <unistd.h>      // close



const std::string RFC_REPLACEMENT = "7931833";



// Send a small HTTP error response to the client
void sendError(int clientFd, int code, const std::string& reason) {
    std::string body = "<html><body><h1>" + std::to_string(code) + " " + reason + "</h1></body></html>";
    std::string response =
        "HTTP/1.1 " + std::to_string(code) + " " + reason + "\r\n"
        "Content-Type: text/html\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n"
        "Connection: close\r\n"
        "\r\n" + body;
    send(clientFd, response.c_str(), response.size(), MSG_NOSIGNAL);
}

// send() may only send part of the data, so loop until it's all gone
bool sendAll(int fd, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        ssize_t n = send(fd, data.c_str() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) return false;
        sent += n;
    }
    return true;
}


// Reads until the other side closes the connection
std::string recvAll(int fd) {
    std::string data;
    char buf[4096];
    ssize_t n;
    while ((n = recv(fd, buf, sizeof(buf), 0)) > 0) {
        data.append(buf, n);
    }
    return data;
}


// Returns the value of a header (e.g. "Host"), or "" if it's missing
std::string getHeader(const std::string& request, const std::string& name) {
    std::string key = "\r\n" + name + ":";
    size_t start = request.find(key);
    if (start == std::string::npos) return "";   // npos = "not found"
    start += key.size();
    size_t end = request.find("\r\n", start);
    std::string value = request.substr(start, end - start);
    size_t first = value.find_first_not_of(' ');  // skip the space after ':'
    return (first == std::string::npos) ? "" : value.substr(first);
}


// Rewrites the Content-Length header to the new body size (if the header exists)
std::string updateContentLength(const std::string& headers, size_t newLength) {
    std::string key = "\r\nContent-Length:";
    size_t start = headers.find(key);
    if (start == std::string::npos) return headers;   // none: client reads until close
    size_t valueStart = start + key.size();
    size_t lineEnd = headers.find("\r\n", valueStart);
    return headers.substr(0, valueStart) + " " + std::to_string(newLength) + headers.substr(lineEnd);
}



// Connects to host:port. Returns the socket, or -1 on failure.
int connectToServer(const std::string& host, int port) {
    addrinfo hints{};
    hints.ai_family = AF_INET;        // IPv4
    hints.ai_socktype = SOCK_STREAM;  // TCP

    addrinfo* results = nullptr;
    int err = getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &results);
    if (err != 0) {
        std::cerr << "DNS lookup failed for " << host << ": " << gai_strerror(err) << std::endl;
        return -1;
    }

    // A host can have several IPs; try each until one connects
    int serverFd = -1;
    for (addrinfo* r = results; r != nullptr; r = r->ai_next) {
        serverFd = socket(r->ai_family, r->ai_socktype, r->ai_protocol);
        if (serverFd < 0) continue;
        if (connect(serverFd, r->ai_addr, r->ai_addrlen) == 0) break;  // success
        close(serverFd);
        serverFd = -1;
    }
    freeaddrinfo(results);   // getaddrinfo allocated memory; give it back
    return serverFd;
}



// Builds the request the proxy sends to the real server
std::string buildServerRequest(const std::string& request, const std::string& path) {
    std::string out = "GET " + path + " HTTP/1.0\r\n";

    size_t pos = request.find("\r\n") + 2;         
    size_t headersEnd = request.find("\r\n\r\n");   

    //Loop through each header line, dropping some and keeping others
    while (pos < headersEnd) {
        size_t lineEnd = request.find("\r\n", pos);
        std::string line = request.substr(pos, lineEnd - pos);
        pos = lineEnd + 2;

        // Header name = text before ':', lowercased (header names are case-insensitive)
        std::string name = line.substr(0, line.find(':'));
        std::transform(name.begin(), name.end(), name.begin(), ::tolower);

        // drop these proxy related headers
        if (name == "proxy-connection" || name == "connection" ||
            name == "keep-alive" || name == "accept-encoding") {
            continue;                                
        }
        out += line + "\r\n";                
    }

    out += "Connection: close\r\n\r\n";              // our own header + blank line
    return out;
}



// Replaces every "RFC###" or "RFC ###" (exactly 3 digits) in text.
// Returns how many replacements were made.
int replaceRfcNumbers(std::string& text, const std::string& replacement) {
    int count = 0;
    size_t pos = 0;
    while ((pos = text.find("RFC", pos)) != std::string::npos) {
        size_t d = pos + 3;                                  // just after "RFC"
        if (d < text.size() && text[d] == ' ') d++;          // optional space

        bool threeDigits = d + 3 <= text.size() &&
                           isdigit((unsigned char)text[d]) &&
                           isdigit((unsigned char)text[d + 1]) &&
                           isdigit((unsigned char)text[d + 2]);
        bool fourthDigit = d + 3 < text.size() && isdigit((unsigned char)text[d + 3]);

        if (threeDigits && !fourthDigit) {
            text.replace(pos, d + 3 - pos, replacement);     // swap "RFC 821" for the digits
            pos += replacement.size();                       // continue after what we inserted
            count++;
        } else {
            pos += 3;                                        // not a match; keep searching
        }
    }
    return count;
}


int main() {
    const int PORT = 11833;  
    // Create a TCP socket
    int listenFd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd < 0) { perror("socket"); return 1; }

    // To prevent "Address already in use" error
    int opt = 1;
    setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // Bind it to our port 
    sockaddr_in addr{};                  // {} zero-initializes the struct
    addr.sin_family = AF_INET;           // IPv4
    addr.sin_addr.s_addr = INADDR_ANY;   // any local IP
    addr.sin_port = htons(PORT);         // htons: convert to network byte order
    if (bind(listenFd, (sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }

    // Start listening
    if (listen(listenFd, 10) < 0) { perror("listen"); return 1; }
    std::cout << "Listening on port " << PORT << std::endl;

    // Handle clients one at a time, forever
    while (true) {
        int clientFd = accept(listenFd, nullptr, nullptr);
        if (clientFd < 0) { perror("accept"); continue; }
        std::cout << "Client connected" << std::endl;

        // Read until the end of the headers (the recv loop from before)
        std::string request;
        char buf[4096];
        while (request.find("\r\n\r\n") == std::string::npos) {
            ssize_t n = recv(clientFd, buf, sizeof(buf), 0);
            if (n <= 0) break;           // 0 = client closed, -1 = error
            request.append(buf, n);
        }

        if (request.empty()) { close(clientFd); continue; }

        if (request.find("\r\n\r\n") == std::string::npos) {   // incomplete headers
            sendError(clientFd, 400, "Bad Request");
            close(clientFd);
            continue;
        }

        // --- Parse the request line "GET http://x/ HTTP/1.1"
        std::string requestLine = request.substr(0, request.find("\r\n"));
        std::istringstream iss(requestLine);
        std::string method, url, version;
        iss >> method >> url >> version;          // splits on spaces
        std::cout << "Request: " << requestLine << std::endl;

        // --- Only GET is supported
        if (method != "GET") {
            std::cout << "Rejected unsupported method: " << method << std::endl;
            sendError(clientFd, 501, "Not Implemented");
            close(clientFd);
            continue;
        }

        // --- Destination host (and optional port) from the Host header
        std::string host = getHeader(request, "Host");
        if (host.empty()) {
            std::cout << "Rejected: no Host header" << std::endl;
            sendError(clientFd, 400, "Bad Request");
            close(clientFd);
            continue;
        }
        int serverPort = 80;
        size_t colon = host.find(':');
        if (colon != std::string::npos) {          // e.g. "example.com:8080"
            serverPort = std::stoi(host.substr(colon + 1));
            host = host.substr(0, colon);
        }

        // --- Turn "http://httpforever.com/page" into "/page"
        std::string path = url;
        if (path.rfind("http://", 0) == 0) {       // starts with "http://"
            size_t slash = path.find('/', 7);      // first '/' after the host
            path = (slash == std::string::npos) ? "/" : path.substr(slash);
        }

        std::cout << "Host: " << host << "  Port: " << serverPort
                  << "  Path: " << path << std::endl;

        std::string serverRequest = buildServerRequest(request, path);

        // --- Connect to the real server
        int serverFd = connectToServer(host, serverPort);
        if (serverFd < 0) {
            std::cout << "Could not reach " << host << std::endl;
            sendError(clientFd, 502, "Bad Gateway");
            close(clientFd);
            continue;
        }
        std::cout << "Connected to " << host << ":" << serverPort << std::endl;

        // --- Forward the request and read the full response
        if (!sendAll(serverFd, serverRequest)) {
            sendError(clientFd, 502, "Bad Gateway");
            close(serverFd);
            close(clientFd);
            continue;
        }
        std::string response = recvAll(serverFd);
        close(serverFd);

        // --- Split the response into headers and body, then rewrite the body
        size_t split = response.find("\r\n\r\n");
        if (split != std::string::npos) {
            std::string headers = response.substr(0, split + 4);   // includes the blank line
            std::string body = response.substr(split + 4);

            std::string contentType = getHeader(headers, "Content-Type");
            if (contentType.find("text/") != std::string::npos) {   // only modify text
                int replaced = replaceRfcNumbers(body, RFC_REPLACEMENT);
                if (replaced > 0) {
                    std::cout << "Replaced " << replaced << " RFC occurrence(s)" << std::endl;
                    headers = updateContentLength(headers, body.size());
                }
            }
            response = headers + body;
        }
        std::cout << "Received " << response.size() << " bytes, status: "
                  << response.substr(0, response.find("\r\n")) << std::endl;

        // --- Send the response back to the client
        sendAll(clientFd, response);
        close(clientFd);
        std::cout << "Transaction complete, sockets closed" << std::endl;  
    }
}