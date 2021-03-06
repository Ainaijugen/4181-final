# Final Project

Weifan Jiang (wj2301@columbia.edu)<br />
Haotang Liu (hl3311@columbia.edu)<br />
Yuan Xu (yx2537@columbia.edu)

A demonstration video of our project is available [here](https://drive.google.com/file/d/1zu6zik90NNRe8OgDMNwe5dj2UnAOqz_I/view?usp=sharing).
Lionmail account is required.

## Install

### install the project

1. Setting up the ca: run `./setupca.sh`
2. Under `CAserver` folder
   1. Run `./setcaserverkeypair.sh`
   2. Run `make`
3. Under `server` folder, 
   1. Run `./setmailserverkeypair.sh`
   2. Run `make`
4. Under `client` folder
   1. Run `./getcacert.sh`
   2. To install a client for a user, run `make install USER=<username>`. For example, run `make install USER=overrich` to get a client for `overrich`. There will be a `client-overrich` under the parent folder. Create more than 1 client for testing.
   
Note: the `CAserver/config`, `server/config` and `client/config` files contain the ip addresses and por

numbers that each component hosts on and/or connects to. The current configurations allow three components
to run on one VM. If need to run on separate VMs, it is necessary to change the configuration files with
appropriate ip addresses and port numbers, and the ip/port for the same component in different files
must match. Once the project is compiled, three components: "`client/`", "`server/`", "`CAserver` and `ca`"
can be moved to different VMs.

### required packages

The following commands can install required packages for the project that are not included in the
default Google Cloud VM.

```
sudo apt-get install build-essential
sudo apt-get install -y libssl-dev
sudo apt install whois
```

## Design

### sandboxing

The program is divided into three components: client, mailing server, and CA server. The client sends requests
to mailing server, and mailing server sends request to CA server for password verification and certificate
generation. The three components are designed to be placed on separate VMs and communicate with HTTPS. The
list of users and hashed passwords are stored with the CA. This architecture ensures that if the mailing
server is compromised, the passwords and the CA private key will not leak.

### encryption/decryption

The general idea of our mail encryption/decryption and authenticity verification:
- We use the symmetric encryption algorithm to encrypt/decrypt messages since: 
  - symmetric algorithm is faster
  - OpenSSL does not support asymmetric algorithm over large files.
- We encrypt the key for the symmetric algorithm using the recipient's public key to utilize the advantage of the asymmetric encryption algorithm. Thus the recipient could decrypt it using its private key.
- For each <sender, recipient> pair, we maintain a unique id for every message. This pair could prevent the server from delivering the same one-time message to the sender again and again.
- For authenticity verification, the sender will sign the mail using its private key while the recipient will verify it using the sender's public key. Thus, we include the sender's client certificate in our mail, which will indicate the sender's identity and generate its public key. This sender's certificate also needs to be encrypted. Otherwise, chances could be that someone else (for example, the server) substitutes its certificate and the mail's signature, and the recipient cannot sense it.
- To sum up, our mail include 3 parts: (a) encrypted key (b) [id | encrypt(cert | msg)] (c) sign( [id | encrypt(cert | msg)] ). When receiving a mail, the receiver first decrypts the key for encryption, decrypts the sender's cert and message, and finally checks the id for this mail and verifies the signature.

The login logic of users using its client certificate:
- The user sends its certificate to the server. The server could verify the authenticity of the certificate by comparing it with the one it previously recorded.
- The server encrypts a random number using the public key obtained from the cert and sends it to the user to confirm that the user is the one who actually holds the certificate.
- Only those with the corresponding private key could decrypt the number correctly, which means the user could verify its authenticity by sending the correct original random number to the server.

### detailed steps
1. `sendmsg`
- The client (sender) sends its certificate, a list of recipient names, and a message to the server.
- The server verifies the sender's certificate using the ca's certificate, then uses the public key derived from - the sender's certificate to encrypt a random number r and send the encrypted random number e(r) to the sender.
- The sender uses its private key to decrypt e(r), send r to the server.
- The server checks if the random number received matches the one it sent before, sends all the recipient's certificates to the sender.
- For each recipient, the sender use the recipient's public key from the certificate to encrypt a symmetric key, sends (a) encrypted key (b) [id | encrypt(cert | msg)] (c) sign( [id | encrypt(cert | msg)] ) 3 components to the mail server.

2. `recvmsg`
- The client (recipient) sends its certificate to the server.
- The server verifies the recipient certificate using the ca's certificate and then checks if the certificate matches the one stored on the server.
- "Random number identity verification"
- The server sends the message package to the recipient
- The recipient uses its private key to decrypt the symmetric key, checks the message's id, decrypt the message using the symmetric key, checks the sender's certificate, and check the signature using the sender's public key.

3. `getcert`
- The client generates a CSR, sends username, password, and CSR to the server.
- The server checks if the user's mailbox is empty, then forwards username, password, and CSR to CAserver.
- The CA server checks the user-password database; if it matches, it sends the user's certificate to the server; otherwise, it sends back an error code.
- The server gets the CA server's response, updates its certificate database, and sends the certificate to the user.

4. `changepw`
- The client sends the username, old password, and new password to the server.
- The server checks if the user's mailbox is empty, then forwards username, old password, and new CSR to CAserver.
- CA server checks and updates its user-password database, generates a new certificate and responds to the server.
- The server gets the CA server's response, updates its certificate database, and sends the new certificate to the user.

## File layout


      4181-final/
      ├── CAserver
      │   ├── CAserver.cpp
      │   ├── Makefile
      │   ├── clear_password_db.sh
      │   ├── config
      │   ├── initial_users.txt
      │   ├── password_permissions.sh
      │   ├── setcaserverkeypair.sh
      │   └── sgencert.sh
      ├── README.md
      ├── client
      │   ├── Makefile
      │   ├── cgencsr.sh
      │   ├── changepw.cpp
      │   ├── client_helper.hpp
      │   ├── config
      │   ├── getcacert.sh
      │   ├── getcert.cpp
      │   ├── openssl.cnf
      │   ├── recvmsg.cpp
      │   ├── sendmsg.cpp
      │   └── test.txt
      ├── server
      │   ├── Makefile
      │   ├── config
      │   ├── create-folders.sh
      │   ├── server.cpp
      │   └── setmailserverkeypair.sh
      └── setupca.sh

## File permission decisions

On the CA side, the passwords are saved as `user_passwords.txt`, and the executable `CAserver` hosts a
HTTPS server and handles requests from the mailing server.

The permission of `user_passwords.txt` is set as follows:

```
-rw-rw---- 1 root CAserver_D6ijQa 4123 Dec 24 04:43 user_passwords.txt
```

The permission of the `CAserver` executable is set as follows:

```
-rwxrwsr-x 1 wj2301 CAserver_D6ijQa 171112 Dec 24 04:43 CAserver
```

The group name `CAserver_D6ijQa` is randomly generated when setting up the server. These permission settings
ensure that on the VM which the CA is hosted on,  only the CA server application and root can read and modify
the password database.

## Testing

1. Under `CAserver` folder
   1. Run `./CAserver`
2. Under `server` folder, 
   1. Run `./server`
   
### 1. all good

1. Install 3 clients: overrich, unrosed and addleness
2. Generate certificates for the two users:
   1. Under `client-overrich`, run `./getcert overrich Freemasonry_bruskest`
      1. Test  `changepw`: run `./changepw overrich Freemasonry_bruskest 123`
   2. Under `client-unrosed`, run `./getcert unrosed shamed_Dow`
   3. Under `client-addleness`, run `./getcert addleness Cardin_pwns`
3. Send message from `overrich` to `unrosed` and `addleness`
   1. Under `client-overrich`, run `./sendmsg unrosed addleness test.txt`
   2. Under `client-unrosed`, run `./recvmsg`
   3. Under `client-addleness`, run `./recvmsg`

### 2. wrong password
1. Install a client for `addleness`
2. Under `client-addleness`, run `./getcert unrosed <wrong password>`

### 3. getcert and changepw with not empty mailbox
1. Install two clients for `overrich` and `unrosed`
2. `getcert` for `overrich` and `unrosed`
3. `overrich` sends a message to `unrosed`
4. run `getcert` and `changepw` under `unrosed`
5. `unrosed` cannot get a new certificate or change its password

### 4. one of the recipients doesn't have a certificate
1. Install 3 clients: overrich, unrosed and addleness
2. Generate certificates for the overrich and unrosed
3. Send message from `overrich` to `unrosed` and `addleness`
   1. Under `client-overrich`, run `./sendmsg unrosed addleness test.txt`
   2. Under `client-unrosed`, run `./recvmsg`
4. `addleness` can still `getcert` and `changepw`. Its mailbox on the server is empty

### 5. server delivers the same mail for multiple times
1. Install 3 clients: `overrish`, `unrosed`
2. Generate certificates for the `overrich` and `unrosed`
3. Under `client-overrich`, run `./sendmsg unrosed test.txt`
4. Under `server/messages/unrosed`, run `cp -r 00000 00001`
5. Under `client-unrosed`, run `./recvmsg` for 2 times. The second mail would show "id corrupted."

