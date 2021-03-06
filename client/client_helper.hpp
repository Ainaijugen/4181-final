#include <memory>
#include <stdarg.h>
#include <stdexcept>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <iostream>
#include <fstream>
#include <sstream>
#include <assert.h>
#include <map>
#include <ctype.h>

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

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

    void send_http_request(BIO *bio, const std::string& line, const std::string& host)
    {
        std::string request = line + "\r\n";
        request += "Host: " + host + "\r\n";
        request += "\r\n";

        BIO_write(bio, request.data(), request.size());
        BIO_flush(bio);
    }

    std::string generate_header(size_t bodylen) {
        std::string request = "POST / HTTP/1.1\r\n";
        request += "Host: duckduckgo.com\r\n";
        request += "Content-Type: application/octet-stream\r\n";
        request += "Content-Length: " + std::to_string(bodylen) + "\r\n";
        request += "\r\n";
        return request;
    }
    
    void check_body(std::string & body) {
        if (body.size() < 4 || body.substr(body.size() - 4, 4) != "\r\n\r\n")
            body += "\r\n\r\n";
    }

    void send_getcert_request(BIO *bio,
                              const std::string& username,
                              const std::string& password,
                              const std::string& csr_content)
    {
        std::string fields = "type=getcert&username=" + username + "&password=" + password;
        std::string body = fields + "\r\n" + csr_content;
        // check_body(body); When sending cert, we do not add \r\n at the end.
        std::string request = my::generate_header(body.size()) + body;

        std::cout << request << std::endl;

        BIO_write(bio, request.data(), request.size());
        BIO_flush(bio);
    }

    void send_changepw_request(BIO *bio,
                               const std::string& username,
                               const std::string& old_password,
                               const std::string& new_password,
                               const std::string& csr_content)
    {
        std::string fields = "type=changepw&username=" + username + "&old_password=" + old_password + "&new_password=";
        fields += new_password;
        std::string body = fields + "\r\n" + csr_content;
        // check_body(body); When sending cert, we do not add \r\n at the end.
        std::string request = my::generate_header(body.size()) + body;
        std::cout << request << std::endl;

        BIO_write(bio, request.data(), request.size());
        BIO_flush(bio);
    }

    void send_certificate(BIO *bio, const std::string & cert_path, const std::string & request_type) {
        std::ifstream cert(cert_path.c_str(), std::ios::binary);
        std::string c((std::istreambuf_iterator<char>(cert)), std::istreambuf_iterator<char>());
        cert.close();
        std::string fields = "type=" + request_type;
        std::string body = fields + "\r\n" + c;
        // check_body(body); When sending cert, we do not add \r\n at the end.
        std::string request = my::generate_header(body.size()) + body;
        BIO_write(bio, request.data(), request.size());
        BIO_flush(bio);
    }

    std::string get_error_code_from_header(const std::string & header) {
        std::size_t fir_ws = header.find(" "); 
        std::size_t sec_ws = header.find(" ", fir_ws + 1); 
        return header.substr(fir_ws + 1, sec_ws - (fir_ws + 1));
    }

    std::string get_error_code_from_file(const std::string & filename) {
        std::stringstream ss(filename);
        std::string temp;
        std::getline(ss,temp);
        return get_error_code_from_header(temp);
    }

    // Return the error_code 
    std::string get_body_and_store(const std::string & response, const std::string & loc) {
        std::stringstream ss(response);
        std::string temp;
        std::getline(ss,temp);
        std::string ret = get_error_code_from_header(temp);
        std::getline(ss,temp);
        std::getline(ss,temp);
        std::ofstream rbody(loc.c_str(), std::ofstream::binary);
        rbody << ss.rdbuf();
        rbody.close();
        return ret;
    }

    void send_number(BIO *bio, const std::string & number) {
        std::string fields = number;
        check_body(fields);
        std::string request = my::generate_header(fields.size());
        request += fields;
        BIO_write(bio, request.data(), request.size());
        BIO_flush(bio);
    }

    void send_number_and_recipient(BIO *bio,
                                   const std::string & number,
                                   std::vector<std::string> recipients) {
        std::string fields = number;
        for (int i = 0; i < recipients.size(); i++) {
            fields += " " + recipients[i];
        }
        check_body(fields);
        std::string request = my::generate_header(fields.size()) + fields;
        BIO_write(bio, request.data(), request.size());
        BIO_flush(bio);
    }

    void check_response(const std::string & loc, std::string error_code) {
        std::ifstream f(loc);
        std::string s;
        f >> s;
        f.close();
        if (error_code != "200") {
            std::cout << "HTTP error code: " << error_code << std::endl;
            std::cout << s << std::endl;
            exit(1);
        }
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

    std::map<std::string, std::string> load_config()
    {
        std::map<std::string, std::string> config_map;
        std::ifstream in("config");
        std::string str;
        while (std::getline(in, str))
        {
            if(str.size() > 0)
            {
                size_t pos = str.find(": ");
                std::string key = str.substr(0, pos);
                std::string value = str.substr(pos + 2, str.size() - pos - 2);
                if (value.back()=='\n') value.pop_back();
                config_map[key] = value;
            }
        }
        return config_map;
    }

    bool is_username_valid(std::string username)
    {
        for (int i = 0; i < username.size(); i ++) {
            if (!islower(username.c_str()[i])) {
                return false;
            }
        }
        return true;
    }

} // namespace my
