/* Bounded first-error text allowlist. No USB, key, image or calibration bytes. */
#pragma once
#include <gio/gio.h>
#include <string.h>
static const char *candidate_safe_message(const GError *error)
{
    static const char *known[]={
        "Invalid outer framing/checksum", "Invalid outer packet", "Invalid protocol checksum",
        "Invalid complete protocol framing", "Command data arrived without its validated ACK",
        "Configuration completion invalid or concurrent configuration loss",
        "Acquisition configuration unavailable", "Image plaintext length or CRC failed",
        "TLS handshake on an unexpected command or before ACK", "Goodix transfer canceled",
        "Receive buffer limit exceeded or null input", "Image TLS forwarding or configuration failed"};
    if(!error || !error->message)return "none";
    for(gsize i=0;i<G_N_ELEMENTS(known);i++)if(!strcmp(error->message,known[i]))return known[i];
    return "unrecognized-message-redacted";
}
static const char *candidate_error_domain(const GError *error)
{
    if(!error)return "none";
    if(error->domain==G_IO_ERROR)return "g-io-error-quark";
    if(error->domain==G_FILE_ERROR)return "g-file-error-quark";
    if(error->domain==G_TLS_ERROR)return "g-tls-error-quark";
    return "other-domain-redacted";
}
