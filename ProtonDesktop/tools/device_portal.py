#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Read-only Device Portal access using a user-supplied root and exact leaf pin.

The Xbox's current leaf signature verifies under its supplied root public key,
but its AKI issuer serial refers to another incarnation of that root. Ordinary
chain building therefore fails. This client first inspects the public TLS leaf
without sending any HTTP bytes, validates that certificate cryptographically
against the supplied root, and then uses a separate CERT_REQUIRED context with
an exact leaf trust anchor, hostname validation and partial-chain support.

No system trust changes, credentials, browser state, cookies, HTTP redirects,
deployment or launch operations are supported. Requires cryptography >= 42.
"""
from __future__ import annotations

import argparse
import datetime as dt
import http.client
import ipaddress
import json
from pathlib import Path
import socket
import ssl
from typing import Any

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import dsa, ec, ed25519, ed448, rsa
from cryptography.x509.oid import ExtendedKeyUsageOID

READ_ONLY_PATHS = frozenset(('/api/os/info', '/api/app/packagemanager/packages'))
MAX_RESPONSE_BYTES = 4 * 1024 * 1024


class PortalHttpError(RuntimeError):
    def __init__(self, status: int, reason: str):
        self.status = status
        super().__init__(f'Device Portal returned HTTP {status} {reason}')


def _load_certificate(data: bytes) -> x509.Certificate:
    return (x509.load_pem_x509_certificate(data) if b'-----BEGIN CERTIFICATE-----' in data
            else x509.load_der_x509_certificate(data))


def _verify_signature(certificate: x509.Certificate, key: Any) -> None:
    if isinstance(key, rsa.RSAPublicKey):
        key.verify(certificate.signature, certificate.tbs_certificate_bytes,
                   certificate.signature_algorithm_parameters, certificate.signature_hash_algorithm)
    elif isinstance(key, ec.EllipticCurvePublicKey):
        key.verify(certificate.signature, certificate.tbs_certificate_bytes,
                   ec.ECDSA(certificate.signature_hash_algorithm))
    elif isinstance(key, dsa.DSAPublicKey):
        key.verify(certificate.signature, certificate.tbs_certificate_bytes,
                   certificate.signature_hash_algorithm)
    elif isinstance(key, (ed25519.Ed25519PublicKey, ed448.Ed448PublicKey)):
        key.verify(certificate.signature, certificate.tbs_certificate_bytes)
    else:
        raise ValueError('Unsupported Device Portal certificate public-key algorithm')


def _check_validity(certificate: x509.Certificate, label: str) -> None:
    now = dt.datetime.now(dt.timezone.utc)
    if not certificate.not_valid_before_utc <= now <= certificate.not_valid_after_utc:
        raise ValueError(f'{label} certificate is not currently valid')


def _check_root(root: x509.Certificate) -> None:
    _check_validity(root, 'User-supplied root')
    if root.subject != root.issuer:
        raise ValueError('The supplied trust anchor must be the self-signed Device Portal root')
    if not root.extensions.get_extension_for_class(x509.BasicConstraints).value.ca:
        raise ValueError('The supplied root is not a CA certificate')
    if not root.extensions.get_extension_for_class(x509.KeyUsage).value.key_cert_sign:
        raise ValueError('The supplied root cannot sign certificates')
    for extension in root.extensions:
        if isinstance(extension.value, x509.NameConstraints) or (
                extension.critical and not isinstance(extension.value, (x509.BasicConstraints, x509.KeyUsage))):
            raise ValueError('The supplied root has constraints this narrow exact-leaf client does not implement')
    _verify_signature(root, root.public_key())


def _check_leaf(leaf: x509.Certificate, root: x509.Certificate) -> dict[str, Any]:
    _check_validity(leaf, 'Device Portal leaf')
    if leaf.issuer != root.subject:
        raise ValueError('The Device Portal leaf issuer does not match the supplied root subject')
    if leaf.extensions.get_extension_for_class(x509.BasicConstraints).value.ca:
        raise ValueError('The Device Portal server leaf unexpectedly declares itself a CA')
    if ExtendedKeyUsageOID.SERVER_AUTH not in leaf.extensions.get_extension_for_class(x509.ExtendedKeyUsage).value:
        raise ValueError('The Device Portal leaf is not valid for TLS server authentication')
    if not leaf.extensions.get_extension_for_class(x509.KeyUsage).value.digital_signature:
        raise ValueError('The Device Portal leaf cannot sign its TLS handshake')
    # Signature verification is the trust decision; identical names/key IDs
    # alone are never accepted as evidence that the server owns a trusted key.
    _verify_signature(leaf, root.public_key())
    authority = leaf.extensions.get_extension_for_class(x509.AuthorityKeyIdentifier).value
    subject = root.extensions.get_extension_for_class(x509.SubjectKeyIdentifier).value
    if authority.key_identifier is not None and authority.key_identifier != subject.digest:
        raise ValueError('Device Portal authority key ID contradicts the supplied root')
    return {
        'root_sha256': root.fingerprint(hashes.SHA256()).hex(),
        'leaf_sha256': leaf.fingerprint(hashes.SHA256()).hex(),
        'root_self_signature_verified': True,
        'leaf_signature_verified_by_supplied_root': True,
        'root_and_leaf_currently_valid': True,
        'authority_serial_matches_root': authority.authority_cert_serial_number == root.serial_number,
        'leaf_has_nonpositive_serial': leaf.serial_number <= 0,
        'verification': 'CERT_REQUIRED; hostname; exact leaf anchor; partial chain',
        'trust_store_modified': False,
        'credentials_sent': False,
    }


class VerifiedPortalClient:
    """Fresh verified HTTPS connection per allowlisted GET; no implicit auth."""

    def __init__(self, root_certificate: Path, host: str,
                 port: int = 11443, timeout: float = 10):
        # Scope this tool to an explicit IP endpoint, not URLs/userinfo/proxies.
        self.host = str(ipaddress.ip_address(host))
        if not 1 <= port <= 65535 or not 0 < timeout <= 60:
            raise ValueError('Invalid Device Portal port or timeout')
        self.port, self.timeout = port, timeout
        root = _load_certificate(root_certificate.read_bytes())
        _check_root(root)

        # PUBLIC CERTIFICATE INSPECTION ONLY: no HTTP request, credentials or
        # cookies are sent over this initial unverified handshake.
        inspection = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        inspection.check_hostname = False
        inspection.verify_mode = ssl.CERT_NONE
        with socket.create_connection((self.host, self.port), self.timeout) as raw:
            with inspection.wrap_socket(raw, server_hostname=self.host) as tls:
                self.leaf_der = tls.getpeercert(binary_form=True)
        leaf = x509.load_der_x509_certificate(self.leaf_der)
        self.verification = _check_leaf(leaf, root)

        self.context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        self.context.check_hostname = True
        self.context.verify_mode = ssl.CERT_REQUIRED
        self.context.load_verify_locations(cadata=leaf.public_bytes(serialization.Encoding.PEM).decode('ascii'))
        self.context.verify_flags |= ssl.VERIFY_X509_PARTIAL_CHAIN

    def get_json(self, path: str) -> dict[str, Any]:
        if path not in READ_ONLY_PATHS:
            raise ValueError('This client permits only the two explicitly scoped read-only GET endpoints')
        connection = http.client.HTTPSConnection(self.host, self.port, timeout=self.timeout,
                                                  context=self.context)
        try:
            connection.connect()
            if connection.sock.getpeercert(binary_form=True) != self.leaf_der:
                raise ssl.SSLCertVerificationError('Device Portal leaf changed after cryptographic qualification')
            # The separate context above verifies both certificate validity and
            # host/IP identity before any authenticated API could receive data.
            connection.request('GET', path, headers={'Accept': 'application/json', 'Connection': 'close'})
            response = connection.getresponse()
            data = response.read(MAX_RESPONSE_BYTES + 1)
            if response.status != 200:
                raise PortalHttpError(response.status, response.reason)
            if len(data) > MAX_RESPONSE_BYTES:
                raise ValueError('Device Portal response exceeded the bounded JSON size')
            if response.getheader('Content-Type', '').split(';', 1)[0].strip().lower() != 'application/json':
                raise ValueError('Device Portal response was not JSON')
            result = json.loads(data)
            if not isinstance(result, dict):
                raise ValueError('Device Portal JSON response was not an object')
            return result
        finally:
            connection.close()


def package_prerequisites(document: dict[str, Any]) -> dict[str, Any]:
    packages = document.get('InstalledPackages')
    if not isinstance(packages, list) or any(not isinstance(p, dict) for p in packages):
        raise ValueError('Device Portal response omitted the InstalledPackages list')
    proton, vclibs = [], []
    for package in packages:
        full_name = package.get('PackageFullName', '')
        parts = full_name.rsplit('_', 4) if isinstance(full_name, str) else []
        identity = parts[0] if len(parts) == 5 else str(package.get('Name', ''))
        architecture = parts[2].lower() if len(parts) == 5 else ''
        if identity.casefold() == 'protondesktop.native':
            selected = proton
        elif identity.casefold() == 'microsoft.vclibs.140.00' and architecture == 'x64':
            selected = vclibs
        else:
            continue
        # Never expose RegisteredUsers, SIDs or unrelated installed apps.
        selected.append({key: package[key] for key in
                         ('Name', 'PackageFullName', 'PackageFamilyName', 'Version') if key in package})
    return {
        'request_path': '/api/app/packagemanager/packages',
        'http_status': 200,
        'protondesktop_native_returned': bool(proton),
        'protondesktop_native': proton,
        'vclibs140_x64_returned': bool(vclibs),
        'vclibs140_x64': vclibs,
        'inventory_limit': 'Not returned means absent from this response; framework availability is not inferred.',
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root-certificate', type=Path, required=True)
    parser.add_argument('--host', required=True)
    parser.add_argument('--port', type=int, default=11443)
    parser.add_argument('--output', type=Path)
    parser.add_argument('operation', choices=('os-info', 'package-prerequisites'), nargs='?',
                        default='package-prerequisites')
    args = parser.parse_args()
    client = VerifiedPortalClient(args.root_certificate, args.host, args.port)
    if args.operation == 'package-prerequisites':
        result = package_prerequisites(client.get_json('/api/app/packagemanager/packages'))
    else:
        result = {'request_path': '/api/os/info', 'http_status': 200, 'os_info': client.get_json('/api/os/info')}
    result['tls'] = client.verification
    encoded = json.dumps(result, indent=2) + '\n'
    if args.output:
        args.output.write_text(encoded)
    print(encoded, end='')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
