// Usage: ./getcert username password public_key_file certificate_file

#include "client_helper.hpp"

int main(int argc, char *argv[]) {

    // format request to server
    my::send_http_request(ssl_bio.get(), "GET / HTTP/1.1", "duckduckgo.com");

}

