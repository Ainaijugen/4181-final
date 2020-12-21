#include <memory>
#include <signal.h>
#include <stdexcept>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <unistd.h>
#include <vector>
#include <iostream>
#include <map>
#include <fstream>
#include <streambuf>

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/ssl.h>

/*
 * install requirements: sudo apt-get install -y libssl-dev
 * compile: g++ -o server -std=c++14 server.cpp -lssl -lcrypto
 * test with command=-line: curl --cacert mailserver.cert.pem --resolve duckduckgo.com:8080:127.0.0.1 https://duckduckgo.com:8080/
 */

namespace my {

template<class T> struct DeleterOf;
template<> struct DeleterOf<BIO> { void operator()(BIO *p) const { BIO_free_all(p); } };
template<> struct DeleterOf<BIO_METHOD> { void operator()(BIO_METHOD *p) const { BIO_meth_free(p); } };
template<> struct DeleterOf<SSL_CTX> { void operator()(SSL_CTX *p) const { SSL_CTX_free(p); } };

template<class OpenSSLType>
using UniquePtr = std::unique_ptr<OpenSSLType, DeleterOf<OpenSSLType>>;

my::UniquePtr<BIO> operator|(my::UniquePtr<BIO> lower, my::UniquePtr<BIO> upper)
{
    BIO_push(upper.get(), lower.release());
    return upper;
}

class StringBIO {
    std::string str_;
    my::UniquePtr<BIO_METHOD> methods_;
    my::UniquePtr<BIO> bio_;
public:
    StringBIO(StringBIO&&) = delete;
    StringBIO& operator=(StringBIO&&) = delete;

    explicit StringBIO() {
        methods_.reset(BIO_meth_new(BIO_TYPE_SOURCE_SINK, "StringBIO"));
        if (methods_ == nullptr) {
            throw std::runtime_error("StringBIO: error in BIO_meth_new");
        }
        BIO_meth_set_write(methods_.get(), [](BIO *bio, const char *data, int len) -> int {
            std::string *str = reinterpret_cast<std::string*>(BIO_get_data(bio));
            str->append(data, len);
            return len;
        });
        bio_.reset(BIO_new(methods_.get()));
        if (bio_ == nullptr) {
            throw std::runtime_error("StringBIO: error in BIO_new");
        }
        BIO_set_data(bio_.get(), &str_);
        BIO_set_init(bio_.get(), 1);
    }
    BIO *bio() { return bio_.get(); }
    std::string str() && { return std::move(str_); }
};

[[noreturn]] void print_errors_and_exit(const char *message)
{
    fprintf(stderr, "%s\n", message);
    ERR_print_errors_fp(stderr);
    exit(1);
}

[[noreturn]] void print_errors_and_throw(const char *message)
{
    my::StringBIO bio;
    ERR_print_errors(bio.bio());
    throw std::runtime_error(std::string(message) + "\n" + std::move(bio).str());
}

SSL *get_ssl(BIO *bio)
{
    SSL *ssl = nullptr;
    BIO_get_ssl(bio, &ssl);
    if (ssl == nullptr) {
        my::print_errors_and_exit("Error in BIO_get_ssl");
    }
    return ssl;
}

std::string receive_some_data(BIO *bio)
{
    char buffer[1024];
    int len = BIO_read(bio, buffer, sizeof(buffer));
    if (len < 0) {
        my::print_errors_and_throw("error in BIO_read");
    } else if (len > 0) {
        return std::string(buffer, len);
    } else if (BIO_should_retry(bio)) {
        return receive_some_data(bio);
    } else {
        my::print_errors_and_throw("empty BIO_read");
    }
}

std::vector<std::string> split_headers(const std::string& text)
{
    std::vector<std::string> lines;
    const char *start = text.c_str();
    while (const char *end = strstr(start, "\r\n")) {
        lines.push_back(std::string(start, end));
        start = end + 2;
    }
    return lines;
}

std::string receive_http_message(BIO *bio)
{
    std::string headers = my::receive_some_data(bio);
    char *end_of_headers = strstr(&headers[0], "\r\n\r\n");
    while (end_of_headers == nullptr) {
        headers += my::receive_some_data(bio);
        end_of_headers = strstr(&headers[0], "\r\n\r\n");
    }
    std::string body = std::string(end_of_headers+4, &headers[headers.size()]);
    headers.resize(end_of_headers+2 - &headers[0]);
    size_t content_length = 0;
    for (const std::string& line : my::split_headers(headers)) {
        if (const char *colon = strchr(line.c_str(), ':')) {
            auto header_name = std::string(&line[0], colon);
            if (header_name == "Content-Length") {
                content_length = std::stoul(colon+1);
            }
        }
    }
    while (body.size() < content_length) {
        body += my::receive_some_data(bio);
    }
    return headers + "\r\n" + body;
}

void send_http_response(BIO *bio, const std::string& body)
{
    std::string response = "HTTP/1.1 200 OK\r\n";
    response += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    response += "\r\n";

    BIO_write(bio, response.data(), response.size());
    BIO_write(bio, body.data(), body.size());
    BIO_flush(bio);
}

void verify_the_certificate(SSL *ssl, const std::string& expected_hostname)
{
    int err = SSL_get_verify_result(ssl);
    if (err != X509_V_OK) {
        const char *message = X509_verify_cert_error_string(err);
        fprintf(stderr, "Certificate verification error: %s (%d)\n", message, err);
        exit(1);
    }
    X509 *cert = SSL_get_peer_certificate(ssl);
    if (cert == nullptr) {
        fprintf(stderr, "No certificate was presented by the server\n");
        exit(1);
    }
#if OPENSSL_VERSION_NUMBER < 0x10100000L
    if (X509_check_host(cert, expected_hostname.data(), expected_hostname.size(), 0, nullptr) != 1) {
        fprintf(stderr, "Certificate verification error: X509_check_host\n");
        exit(1);
    }
#else
    // X509_check_host is called automatically during verification,
    // because we set it up in main().
    (void)expected_hostname;
#endif
}

my::UniquePtr<BIO> accept_new_tcp_connection(BIO *accept_bio)
{
    if (BIO_do_accept(accept_bio) <= 0) {
        return nullptr;
    }
    return my::UniquePtr<BIO>(BIO_pop(accept_bio));
}

} // namespace my

std::vector<std::string> splitStringBy(std::string s, std::string delimiter) {
    std::vector<std::string> splitted;
    std::string unparsed(s);
    size_t pos = 0;
    std::string token;
    while ((pos = unparsed.find(delimiter)) != std::string::npos) {
        token = unparsed.substr(0, pos);
        splitted.push_back(token);
        unparsed.erase(0, pos + delimiter.length());
    }
    splitted.push_back(unparsed);
    return splitted;
}

int main()
{
#if OPENSSL_VERSION_NUMBER < 0x10100000L
    SSL_library_init();
    SSL_load_error_strings();
    auto ctx = my::UniquePtr<SSL_CTX>(SSL_CTX_new(SSLv23_method()));
#else
    auto ctx = my::UniquePtr<SSL_CTX>(SSL_CTX_new(TLS_method()));
    SSL_CTX_set_min_proto_version(ctx.get(), TLS1_2_VERSION);
#endif

    if (SSL_CTX_use_certificate_file(ctx.get(), "mailserver.cert.pem", SSL_FILETYPE_PEM) <= 0) {
        my::print_errors_and_exit("Error loading server certificate");
    }
    if (SSL_CTX_use_PrivateKey_file(ctx.get(), "mailserver.key.pem", SSL_FILETYPE_PEM) <= 0) {
        my::print_errors_and_exit("Error loading server private key");
    }

    auto accept_bio = my::UniquePtr<BIO>(BIO_new_accept("8080"));
    if (BIO_do_accept(accept_bio.get()) <= 0) {
        my::print_errors_and_exit("Error in BIO_do_accept (binding to port 8080)");
    }

    static auto shutdown_the_socket = [fd = BIO_get_fd(accept_bio.get(), nullptr)]() {
        close(fd);
    };
    signal(SIGINT, [](int) { shutdown_the_socket(); });

    while (auto bio = my::accept_new_tcp_connection(accept_bio.get())) {
        bio = std::move(bio)
            | my::UniquePtr<BIO>(BIO_new_ssl(ctx.get(), 0))
            ;
        try {
            std::string request = my::receive_http_message(bio.get());
            printf("Got request:\n");           
            // handle request based on type
            //std::cout << request << std::endl;
            std::vector<std::string> requestLines = splitStringBy(request, "\r\n");

            std::map<std::string, std::string> paramMap;
            std::vector<std::string> params = splitStringBy(requestLines[5], "&");
            for (int i = 0; i < params.size(); i ++) {
                std::vector <std::string> kv = splitStringBy(params[i], "=");
                paramMap[kv[0]] = kv[1];
            }

            if (paramMap["type"].compare("getcert") == 0) {
                std::cout << "getcert request received from user " << paramMap["username"] << std::endl;
                std::string username = paramMap["username"];
                std::string password = paramMap["password"];
#if OPENSSL_VERSION_NUMBER < 0x10100000L
                SSL_library_init();
                SSL_load_error_strings();
#endif

                /* Set up the SSL context */

#if OPENSSL_VERSION_NUMBER < 0x10100000L
                auto ctx = my::UniquePtr<SSL_CTX>(SSL_CTX_new(SSLv23_client_method()));
#else
                auto ctx = my::UniquePtr<SSL_CTX>(SSL_CTX_new(TLS_client_method()));
#endif
                
                // edit this to trust a local certificate
                // if (SSL_CTX_set_default_verify_paths(ctx.get()) != 1) {
                // use the ca's certificate here
                if (SSL_CTX_load_verify_locations(ctx.get(), "ca-chain.cert.pem", nullptr) != 1) {
                    my::print_errors_and_exit("Error setting up trust store");
                }
                auto CAbio = my::UniquePtr<BIO>(BIO_new_connect("localhost:10086"));
                if (CAbio == nullptr) {
                    my::print_errors_and_exit("Error in BIO_new_connect");
                }
                if (BIO_do_connect(CAbio.get()) <= 0) {
                    my::print_errors_and_exit("Error in BIO_do_connect");
                }
                auto CAssl_bio = std::move(CAbio)
                               | my::UniquePtr<BIO>(BIO_new_ssl(ctx.get(), 1))
                ;
                SSL_set_tlsext_host_name(my::get_ssl(CAssl_bio.get()), "luckluckgo.com");
                if (BIO_do_handshake(CAssl_bio.get()) <= 0) {
                    my::print_errors_and_exit("Error in BIO_do_handshake");
                }
                my::verify_the_certificate(my::get_ssl(CAssl_bio.get()), "luckluckgo.com");

                std::string fields = "type=getcert&username=" + username + "&password=" + password;
                std::string request = "POST / HTTP/1.1\r\n";
                request += "Host: duckduckgo.com\r\n";
                request += "Content-Type: application/x-www-form-urlencoded\r\n";
                request += "Content-Length: " + std::to_string(fields.size()) + "\r\n";
                request += "\r\n";
                request += fields + "\r\n";
                request += "\r\n";
                BIO_write(CAssl_bio.get(), request.data(), request.size());
                BIO_flush(CAssl_bio.get());

                std::string response = my::receive_http_message(CAssl_bio.get());
                printf("%s", response.c_str());
                my::send_http_response(bio.get(), "okay cool\n");
            } else if (paramMap["type"].compare("changepw") == 0) {
                std::cout << "changepw request received from user " << paramMap["username"] << std::endl;
                my::send_http_response(bio.get(), "okay cool\n");
            } else if (paramMap["type"].compare("sendmsg") == 0) {
                std::cout << "sendmsg request. certificate get." << std::endl;
                // need to be done: check certificate

                std::string r = std::to_string(rand());  // need to be checked the same!
                std::cout << "sendmsg request. rand number sent is " << r << std::endl;
                // need to change the certificate
                std::ofstream f("num.temp", std::ofstream::binary);
                f << r;
                f.close();
                system("openssl pkeyutl -encrypt -pubin -inkey ../client/cindy.pubkey.pem -in num.temp -out encryp.temp");
                std::ifstream encryptn("encryp.temp", std::ifstream::binary);
                std::string encrypted_r((std::istreambuf_iterator<char>(encryptn)), std::istreambuf_iterator<char>());
                encryptn.close();
                my::send_http_response(bio.get(), encrypted_r);

                // get number and recipient
                //bio = my::accept_new_tcp_connection(accept_bio.get());
                request = my::receive_http_message(bio.get());
                printf("Got request:\n");           
                std::vector<std::string> requestLines = splitStringBy(request, "\r\n");
                std::vector<std::string> para = splitStringBy(requestLines[5], "&");
                std::cout << "sendmsg request. rand number receive is " + para[0] << ", recipient is " << para[1] << std::endl;
                if (para[0] != r) {
                    //std::cout << "Number does not match! Fake identity!!!" << std::endl;
                    my::send_http_response(bio.get(),"Fake identity");
                    continue;
                }
                else {
                    std::cout << "Number match! Identity confirmed!!!" << std::endl;
                }
                // send recipient certificate, need change
                std::ifstream f2("../client/bob.cert.pem", std::ifstream::binary);
                std::string cert((std::istreambuf_iterator<char>(f2)), std::istreambuf_iterator<char>());
                f2.close();
                my::send_http_response(bio.get(),cert);

                // get msg
                request = my::receive_http_message(bio.get());
                printf("Got request:\n");           
                requestLines = splitStringBy(request, "\r\n");
                std::cout << "sendmsg request. msg get " << std::endl ;//<< requestLines[5];
                std::ofstream msg("msgget", std::ofstream::binary);
                msg << requestLines[5];
                msg.close();
                my::send_http_response(bio.get(),"ok");
            }
        } catch (const std::exception& ex) {
            printf("Worker exited with exception:\n%s\n", ex.what());
        }
    }
    printf("\nClean exit!\n");
}
