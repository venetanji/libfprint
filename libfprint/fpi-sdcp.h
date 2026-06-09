/*
 * Secure Device Connection Protocol (SDCP) support implementation
 * Copyright (C) 2025 Joshua Grisham <josh@joshuagrisham.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#pragma once

#include "fpi-compat.h"

#include "fpi-sdcp-device.h"

/**
 * fpi_sdcp_generate_host_key:
 * @private_key: (out) (transfer full): The host private key (sk_h)
 * @public_key: (out) (transfer full): The host public key (pk_h)
 * @error: (out): #GError in case the out values are %NULL
 *
 * Function to generate a new ephemeral ECDH key pair for use with SDCP.
 **/
void fpi_sdcp_generate_host_key (GBytes **private_key,
                                 GBytes **public_key,
                                 GError **error);

/**
 * fpi_sdcp_generate_random:
 * @error: (out): #GError in case the return value is %NULL
 *
 * Returns: A new #GBytes with a secure random of length %SDCP_RANDOM_SIZE
 **/
GBytes *fpi_sdcp_generate_random (GError **error);

/**
 * fpi_sdcp_verify_connect:
 * @host_private_key: Private key generated using fpi_sdcp_generate_host_key() (sk_h)
 * @host_random: Random generated using fpi_sdcp_generate_random() (r_h)
 * @device_random: The random provided in the device's ConnectResponse (r_d)
 * @claim: #FpiSdcpClaim provided in the device's ConnectResponse (c)
 * @mac: The MAC provided in the device's ConnectResponse (m)
 * @validate_certificate: If the model certificate (cert_m) should be parsed and
 *   its trust chain validated as issued from Microsoft's well-known issuers
 * @verify_signatures: If the model signature (s_m) and device signature (s_d)
 *   should be validated against the certificate and keys provided in the claim
 * @application_secret: (out) (transfer full): A new #GBytes with the derived
 *   application secret (s) of length %SDCP_APPLICATION_SECRET_SIZE
 * @error: (out): #GError in case the return value is %NULL
 *
 * High level function which internally handles the derivation of all necessary
 * SDCP-related keys and secrets from the device's ConnectResponse and derives
 * the application secret for use with all other SDCP-related functions.
 *
 * This function will also perform a validation of the ConnectResponse MAC and
 * optionally perform additional verifications based on the provided
 * @validate_certificate and @verify_signatures booleans. If any of these these
 * validations fail then %NULL will be returned, indicating that the SDCP secure
 * connection channel could not be established.
 *
 * Returns: %TRUE if the @application_secret was successfully derived and the
 * ConnectResponse has been successfully verified
 **/
gboolean fpi_sdcp_verify_connect (GBytes       *host_private_key,
                                  GBytes       *host_random,
                                  GBytes       *device_random,
                                  FpiSdcpClaim *claim,
                                  GBytes       *mac,
                                  gboolean      validate_certificate,
                                  gboolean      verify_signatures,
                                  GBytes      **application_secret,
                                  GError      **error);

/**
 * fpi_sdcp_verify_reconnect:
 * @application_secret: The host's derived application secret (s)
 * @random: The host-generated random sent to the device's Reconnect command (r)
 * @mac: The MAC provided in the device's ReconnectResponse (m)
 * @error: (out): #GError in case the return value is %FALSE
 *
 * Verifies the SDCP ReconnectResponse.
 *
 * Returns: %TRUE if the ReconnectResponse is verified successfully
 **/
gboolean fpi_sdcp_verify_reconnect (GBytes  *application_secret,
                                    GBytes  *random,
                                    GBytes  *mac,
                                    GError **error);

/**
 * fpi_sdcp_verify_identify:
 * @application_secret: The host's derived application secret (s)
 * @nonce: The host-generated nonce sent to the device's Identify command (n)
 * @id: The ID provided in the device's AuthorizedIdentity (id)
 * @mac: The MAC provided in the device's AuthorizedIdentity (m)
 * @error: (out): #GError in case the return value is %FALSE
 *
 * Verifies the SDCP ReconnectResponse.
 *
 * Returns: %TRUE if the ReconnectResponse is verified successfully
 **/
gboolean fpi_sdcp_verify_identify (GBytes  *application_secret,
                                   GBytes  *nonce,
                                   GBytes  *id,
                                   GBytes  *mac,
                                   GError **error);

/**
 * fpi_sdcp_generate_enrollment_id:
 * @application_secret: The host's derived application secret (s)
 * @nonce: The nonce received from the device in response to the EnrollBegin
 *   command (n)
 * @error: (out): #GError in case the return value is %NULL
 *
 * Generates a new id for use with the device's EnrollCommit command.
 *
 * Returns: A new #GBytes with the generated enrollment id of length
 * %SDCP_ENROLLMENT_ID_SIZE
 **/
GBytes *fpi_sdcp_generate_enrollment_id (GBytes  *application_secret,
                                         GBytes  *nonce,
                                         GError **error);
