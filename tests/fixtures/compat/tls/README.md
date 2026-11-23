# TLS test certificate

`cert.pem` and `key.pem` are a self-signed certificate for `localhost` and
`127.0.0.1` and its private key, made with `openssl req -x509` for the one
test that checks that the media server client refuses a certificate that
does not verify unless it is told to accept it. The key protects nothing: it
exists only in this repository and is used only by a fake server on
loopback. It is valid for a hundred years.
