/*
 * Virtual driver test payloads for SDCP device debugging
 *
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

/* host keys */

static const gchar host_private_key_hex[] = "8400ed14579cdf11586477e836e8cb52708441c1c2a447c218c5bbc2d118fbc7";
static const gchar host_public_key_hex[]  = "0452f056ffb9c6728654771a3629b770767b19a2106a4916fb81ba06ef6797c4"
                                            "a3df672ade0e9116d1abe278a8223abde4958d62d4ff6882159f0617c6f8ce10"
                                            "bf";
static const gchar host_random_hex[]      = "d877403abe82f4d97e1448c5052d83a532a45e56ef049cbbf981137520e713bf";

/* device keys */

static const gchar device_random_hex[]       = "6e2f6c1abef2a1973fb1315a17e209fdb0c78520f1fd6a85d294d7aeb40a04a7";
static const gchar model_certificate_hex[]   = "30820323308202caa00302010202133300000004c45d661d6eed040d00000000"
                                               "0004300a06082a8648ce3d0403023056310b3009060355040613025553311e30"
                                               "1c060355040a13154d6963726f736f667420436f72706f726174696f6e312730"
                                               "250603550403131e57696e646f77732048656c6c6f2032303936414443432043"
                                               "412032303231301e170d3232303130343231303033355a170d32333034303432"
                                               "31303033355a301c311a3018060355040313115365637572652042494f205365"
                                               "6e736f723059301306072a8648ce3d020106082a8648ce3d0301070342000414"
                                               "cfc287f872a2b7d3339e0b31390e3ca688e61165eaa6687c959270e07666b1fa"
                                               "19e3efaf1750d134a886d494424fe471970c4b06838408a18d1f5d57735dd7a3"
                                               "8201af308201ab30750603551d11046e306ca46a30683132303006082b060104"
                                               "82376402132436423045413344382d383339372d343444332d383043392d3930"
                                               "324130414346344343303132303006082b060104823764011324413944423730"
                                               "32362d433646462d344341462d423134382d393133454641333730423933301d"
                                               "0603551d0e04160414db6d66d642b7236d5e6bb2ec2186decda98067b6301f06"
                                               "03551d23041830168014bf3748e34a632de953a3ba890298c069472a99b9305f"
                                               "0603551d1f045830563054a052a050864e687474703a2f2f7777772e6d696372"
                                               "6f736f66742e636f6d2f706b696f70732f63726c2f57696e646f777325323048"
                                               "656c6c6f25323032303936414443432532304341253230323032312e63726c30"
                                               "6c06082b060105050701010460305e305c06082b060105050730028650687474"
                                               "703a2f2f7777772e6d6963726f736f66742e636f6d2f706b696f70732f636572"
                                               "74732f57696e646f777325323048656c6c6f2532303230393641444343253230"
                                               "4341253230323032312e637274300c0603551d130101ff040230003015060355"
                                               "1d25040e300c060a2b0601040182374c2b01300a06082a8648ce3d0403020347"
                                               "003044022077553ef520f732e03cd740c8cf807e6366e12918bc581f75bfe0f1"
                                               "95b7b1fd4f0220324e25b93b9da7538b797a624272b21b7cc0e96ea487924250"
                                               "4677600450f283";
static const gchar device_public_key_hex[]   = "04e2787890a684f95b96b9a2316ca8d3d33d4d79ff4c89dc6f9e888e973990d1"
                                               "d3154133dcc8bd33b99af9dbf0673390d404d092498a3f214cd93f9b9f28fb5f"
                                               "66";
static const gchar firmware_public_key_hex[] = "04f06a84ab51a3a6e8ff46868f91dd720e4cdad21f2e090d11e8f9bfc2ea19ee"
                                               "1b5eac850b4532968a9399f76cd779e7723e8c2ca73b597c0df5f73b94a36f2b"
                                               "6c";
static const gchar firmware_hash_hex[]       = "c3bf47ea1f4a4a605470313cacb3a44f4a461f68c6faeab07e737610cb5ac835";
static const gchar model_signature_hex[]     = "febe6ba3107813e185f05189e69ae79d9f7a40802582d94324459844c8b97ec6"
                                               "c5daed5462276cb8a193c33e350424b0305d63d79a93a9188dcfc0cb5595f6c1";
static const gchar device_signature_hex[]    = "10cc57dd8dafb463510a7327a5fca49b698e999b36448e2023eaf0dff0b0d4a3"
                                               "4f1caf4e872b77364a0a00d7476554d0324c4cc931937e232a0315837d696c06";
static const gchar connect_mac_hex[]         = "422bc475a78f972bae842a28e5ad721207457fcbd9a1a3aaf71587c07b84d247";

/* expected application_secret based on above */

static const gchar application_secret_hex[] = "13330ba3135ecf5dc71cede01a886540771efab35c8ba053902b2c1ee7de6efe";

/* test verify_reconnect values */

static const gchar reconnect_random_hex[] = "8a7451c1d3a8dca1c1330ca50d73454b351a49f46c8e9dcee15c964d295c31c9";
static const gchar reconnect_mac_hex[]    = "bf3f3bb3bd6ecb2784c160f526f7bc3b3ca8faf5557194c48e0024a0493903c7";

/* test enrollment_id values */

static const gchar enrollment_nonce_hex[] = "c2101c44c9a667bba397e81f48b143398603e2c9335a68b409e1dbe71e005ca2";
static const gchar enrollment_id_hex[]    = "67109dc70a216331f1580ddac601915929c1ff6c9bcba6544ba572c660c3d91e";

/* test verify_identify values */

static const gchar identify_nonce_hex[] = "3a1b506f5bec089059acefb9b44dfbdea7a599ee9aa267e5252664d60b798053";
static const gchar identify_mac_hex[]   = "53a723eef40713094a90c5ef9996cbd6ba268e30676cd7107705a6c3e3e1eff9";
