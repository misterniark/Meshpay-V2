#include "meshpay/currency_descriptor.h"
#include "meshpay/rns/rns_destination.h"
#include "meshpay/rns/rns_identity.h"
#include "unity.h"
#include <string.h>

/*
 * Remplit un corps de descripteur « réaliste ». Le founder_public est laissé à
 * zéro ici : sign() le renseignera depuis l'identité du fondateur. Pour les
 * tests de pur déterminisme d'encode_body, on le force à une valeur fixe.
 */
static void fill_body(meshpay_currency_descriptor_t *body)
{
    meshpay_currency_descriptor_init(body);
    strncpy(body->name, "Minimistan", sizeof(body->name) - 1);
    strncpy(body->symbol, "MIN", sizeof(body->symbol) - 1);
    body->max_supply = 1000000ULL;
    body->transfer_fee = 5;
    body->demurrage_enabled = true;
    body->demurrage_bps = 250;
    body->initial_credit = 100;
    body->created_at_ms = 1716200000123ULL;
}

TEST_CASE("currency descriptor init met tout a zero", "[currency_descriptor]")
{
    meshpay_currency_descriptor_t body;
    /* Pré-salir la structure pour vérifier que init la nettoie réellement. */
    memset(&body, 0xAB, sizeof(body));
    meshpay_currency_descriptor_init(&body);

    meshpay_currency_descriptor_t zero;
    memset(&zero, 0, sizeof(zero));
    TEST_ASSERT_EQUAL_MEMORY(&zero, &body, sizeof(body));
}

TEST_CASE("currency descriptor encode_body deterministe", "[currency_descriptor]")
{
    meshpay_currency_descriptor_t a;
    meshpay_currency_descriptor_t b;
    fill_body(&a);
    fill_body(&b);
    /* founder_public fixe et identique pour les deux corps. */
    for (size_t i = 0; i < sizeof(a.founder_public); ++i) {
        a.founder_public[i] = (uint8_t)i;
        b.founder_public[i] = (uint8_t)i;
    }

    uint8_t buf_a[MESHPAY_CURRENCY_DESCRIPTOR_CBOR_MAX];
    uint8_t buf_b[MESHPAY_CURRENCY_DESCRIPTOR_CBOR_MAX];
    size_t len_a = 0;
    size_t len_b = 0;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_currency_descriptor_encode_body(&a, buf_a,
                                                                      sizeof(buf_a),
                                                                      &len_a));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_currency_descriptor_encode_body(&b, buf_b,
                                                                      sizeof(buf_b),
                                                                      &len_b));
    /* Deux corps identiques -> octets identiques. */
    TEST_ASSERT_EQUAL_size_t(len_a, len_b);
    TEST_ASSERT_EQUAL_MEMORY(buf_a, buf_b, len_a);
    /* En-tête CBOR : map de 9 éléments = 0xA9, première clé = 0x01. */
    TEST_ASSERT_EQUAL_HEX8(0xA9, buf_a[0]);
    TEST_ASSERT_EQUAL_HEX8(0x01, buf_a[1]);

    uint8_t genesis_a[MESHPAY_CURRENCY_GENESIS_SIZE];
    uint8_t genesis_b[MESHPAY_CURRENCY_GENESIS_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_currency_descriptor_compute_genesis(&a, genesis_a, NULL));
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_currency_descriptor_compute_genesis(&b, genesis_b, NULL));
    TEST_ASSERT_EQUAL_MEMORY(genesis_a, genesis_b, sizeof(genesis_a));

    /* Modifier UNE règle (max_supply) doit changer le genesis. */
    b.max_supply = 999999ULL;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_currency_descriptor_compute_genesis(&b, genesis_b, NULL));
    TEST_ASSERT_NOT_EQUAL(0, memcmp(genesis_a, genesis_b, sizeof(genesis_a)));
}

TEST_CASE("currency descriptor currency_id derive du genesis", "[currency_descriptor]")
{
    meshpay_currency_descriptor_t body;
    fill_body(&body);

    uint8_t genesis[MESHPAY_CURRENCY_GENESIS_SIZE];
    uint32_t currency_id = 0;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_currency_descriptor_compute_genesis(&body, genesis,
                                                                  &currency_id));
    /* currency_id = 4 octets de tête du genesis, big-endian. */
    uint32_t expected = ((uint32_t)genesis[0] << 24) |
                        ((uint32_t)genesis[1] << 16) |
                        ((uint32_t)genesis[2] << 8) |
                        ((uint32_t)genesis[3]);
    TEST_ASSERT_EQUAL_UINT32(expected, currency_id);
}

TEST_CASE("currency descriptor sign puis verify reussit", "[currency_descriptor]")
{
    rns_identity_t founder;
    TEST_ASSERT_EQUAL(ESP_OK, rns_identity_generate(&founder));

    meshpay_currency_descriptor_t body;
    fill_body(&body);

    meshpay_currency_descriptor_signed_t signed_desc;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_currency_descriptor_sign(&signed_desc, &body,
                                                               &founder));
    /* sign doit avoir renseigné founder_public depuis l'identité. */
    uint8_t founder_pub[RNS_IDENTITY_PUBLIC_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK, rns_identity_get_public_key(&founder, founder_pub));
    TEST_ASSERT_EQUAL_MEMORY(founder_pub, signed_desc.body.founder_public,
                             sizeof(founder_pub));
    TEST_ASSERT_NOT_EQUAL(0, signed_desc.currency_id);
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_currency_descriptor_verify(&signed_desc));
}

TEST_CASE("currency descriptor signature corrompue rejetee", "[currency_descriptor]")
{
    rns_identity_t founder;
    TEST_ASSERT_EQUAL(ESP_OK, rns_identity_generate(&founder));

    meshpay_currency_descriptor_t body;
    fill_body(&body);

    meshpay_currency_descriptor_signed_t signed_desc;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_currency_descriptor_sign(&signed_desc, &body,
                                                               &founder));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_currency_descriptor_verify(&signed_desc));

    /* Corrompre 1 octet de signature -> la vérification Ed25519 doit échouer. */
    signed_desc.founder_signature[0] ^= 0x55;
    TEST_ASSERT_NOT_EQUAL(ESP_OK,
                          meshpay_currency_descriptor_verify(&signed_desc));
}

TEST_CASE("currency descriptor corps modifie apres signature rejete", "[currency_descriptor]")
{
    rns_identity_t founder;
    TEST_ASSERT_EQUAL(ESP_OK, rns_identity_generate(&founder));

    meshpay_currency_descriptor_t body;
    fill_body(&body);

    meshpay_currency_descriptor_signed_t signed_desc;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_currency_descriptor_sign(&signed_desc, &body,
                                                               &founder));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_currency_descriptor_verify(&signed_desc));

    /* Trafiquer un champ du corps sans re-signer : genesis recalculé != stocké. */
    signed_desc.body.transfer_fee += 1;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      meshpay_currency_descriptor_verify(&signed_desc));
}

TEST_CASE("currency descriptor encode decode round-trip", "[currency_descriptor]")
{
    rns_identity_t founder;
    TEST_ASSERT_EQUAL(ESP_OK, rns_identity_generate(&founder));

    meshpay_currency_descriptor_t body;
    fill_body(&body);

    meshpay_currency_descriptor_signed_t signed_desc;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_currency_descriptor_sign(&signed_desc, &body,
                                                               &founder));

    uint8_t wire[MESHPAY_CURRENCY_DESCRIPTOR_CBOR_MAX];
    size_t wire_len = 0;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_currency_descriptor_encode(&signed_desc, wire,
                                                                 sizeof(wire),
                                                                 &wire_len));
    /* En-tête CBOR : map de 11 éléments = 0xAB. */
    TEST_ASSERT_EQUAL_HEX8(0xAB, wire[0]);

    meshpay_currency_descriptor_signed_t decoded;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_currency_descriptor_decode(wire, wire_len,
                                                                 &decoded));
    /* Tous les champs doivent coïncider à l'octet près. */
    TEST_ASSERT_EQUAL_MEMORY(&signed_desc.body, &decoded.body,
                             sizeof(signed_desc.body));
    TEST_ASSERT_EQUAL_MEMORY(signed_desc.genesis_hash, decoded.genesis_hash,
                             sizeof(signed_desc.genesis_hash));
    TEST_ASSERT_EQUAL_MEMORY(signed_desc.founder_signature,
                             decoded.founder_signature,
                             sizeof(signed_desc.founder_signature));
    TEST_ASSERT_EQUAL_UINT32(signed_desc.currency_id, decoded.currency_id);
    /* Le descripteur décodé doit aussi vérifier. */
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_currency_descriptor_verify(&decoded));
}

TEST_CASE("currency descriptor founder_hash egale hash destination wallet", "[currency_descriptor]")
{
    rns_identity_t founder;
    TEST_ASSERT_EQUAL(ESP_OK, rns_identity_generate(&founder));

    meshpay_currency_descriptor_t body;
    fill_body(&body);

    meshpay_currency_descriptor_signed_t signed_desc;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_currency_descriptor_sign(&signed_desc, &body,
                                                               &founder));

    /* Invariant depuis la revue Palier D (e964b71) : l'autorité MINT est le
     * COMPTE transactionnel du fondateur = hash de sa destination
     * meshpay.wallet (dérivable de founder_public seul), PAS son hash
     * d'identité — sinon les frais de transfert seraient routés vers un compte
     * inaccessible et la frappe fondateur future inautorisable. */
    uint8_t hash_from_desc[RNS_IDENTITY_HASH_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_currency_descriptor_founder_hash(&signed_desc,
                                                               hash_from_desc));
    rns_destination_t founder_wallet;
    TEST_ASSERT_EQUAL(ESP_OK, rns_destination_create_meshpay_wallet(
                                  &founder, &founder_wallet));
    TEST_ASSERT_EQUAL_MEMORY(founder_wallet.hash, hash_from_desc,
                             RNS_IDENTITY_HASH_SIZE);

    /* Et ce n'est PAS le hash d'identité (l'ancien invariant, corrigé). */
    uint8_t identity_hash[RNS_IDENTITY_HASH_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK, rns_identity_get_hash(&founder, identity_hash));
    TEST_ASSERT_TRUE(memcmp(identity_hash, hash_from_desc,
                            sizeof(identity_hash)) != 0);
}

TEST_CASE("currency descriptor taille wire bornee", "[currency_descriptor]")
{
    rns_identity_t founder;
    TEST_ASSERT_EQUAL(ESP_OK, rns_identity_generate(&founder));

    /* Nom et symbole pleins (capacité - 1) pour maximiser la taille encodée. */
    meshpay_currency_descriptor_t body;
    meshpay_currency_descriptor_init(&body);
    memset(body.name, 'A', MESHPAY_CURRENCY_NAME_MAX - 1);
    body.name[MESHPAY_CURRENCY_NAME_MAX - 1] = '\0';
    memset(body.symbol, 'B', MESHPAY_CURRENCY_SYMBOL_MAX - 1);
    body.symbol[MESHPAY_CURRENCY_SYMBOL_MAX - 1] = '\0';
    body.max_supply = UINT64_MAX;
    body.transfer_fee = UINT32_MAX;
    body.demurrage_enabled = true;
    body.demurrage_bps = UINT16_MAX;
    body.initial_credit = UINT32_MAX;
    body.created_at_ms = UINT64_MAX;

    meshpay_currency_descriptor_signed_t signed_desc;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_currency_descriptor_sign(&signed_desc, &body,
                                                               &founder));

    uint8_t wire[MESHPAY_CURRENCY_DESCRIPTOR_CBOR_MAX];
    size_t wire_len = 0;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_currency_descriptor_encode(&signed_desc, wire,
                                                                 sizeof(wire),
                                                                 &wire_len));
    /* Marge confortable sous le MTU LoRa : descripteur signé <= 320 o. */
    TEST_ASSERT_TRUE(wire_len <= 320);
}

/*
 * Cas négatifs du décodeur. Un descripteur arrive par radio (LoRa/ESP-NOW) :
 * decode() est la PREMIÈRE ligne de défense contre un message malveillant. On
 * exerce explicitement chaque garde (sinon une régression qui les désactive
 * passerait verte). Les offsets utilisés sont stables : le founder est le 1er
 * champ (en-tête bstr aux offsets 2/3) et la signature est le dernier champ
 * (clé + bstr64 = 67 octets, donc clé à wire_len-67).
 */
TEST_CASE("currency descriptor decode rejette les wires malformes", "[currency_descriptor]")
{
    rns_identity_t founder;
    TEST_ASSERT_EQUAL(ESP_OK, rns_identity_generate(&founder));

    meshpay_currency_descriptor_t body;
    fill_body(&body);

    meshpay_currency_descriptor_signed_t signed_desc;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_currency_descriptor_sign(&signed_desc, &body,
                                                               &founder));

    uint8_t wire[MESHPAY_CURRENCY_DESCRIPTOR_CBOR_MAX];
    size_t wire_len = 0;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_currency_descriptor_encode(&signed_desc, wire,
                                                                 sizeof(wire),
                                                                 &wire_len));

    meshpay_currency_descriptor_signed_t decoded;

    /* Référence : le wire intact décode bien. */
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_currency_descriptor_decode(wire, wire_len, &decoded));

    /* (a) Wire tronqué d'un octet -> lecture du dernier bstr impossible. */
    TEST_ASSERT_NOT_EQUAL(ESP_OK,
                          meshpay_currency_descriptor_decode(wire, wire_len - 1,
                                                             &decoded));

    /* (b) Octet parasite en fin -> buffer non intégralement consommé. */
    uint8_t surplus[MESHPAY_CURRENCY_DESCRIPTOR_CBOR_MAX + 1];
    memcpy(surplus, wire, wire_len);
    surplus[wire_len] = 0x00;
    TEST_ASSERT_NOT_EQUAL(ESP_OK,
                          meshpay_currency_descriptor_decode(surplus, wire_len + 1,
                                                             &decoded));

    /* (c) En-tête de map faux (10 au lieu de 11). */
    uint8_t tampered[MESHPAY_CURRENCY_DESCRIPTOR_CBOR_MAX];
    memcpy(tampered, wire, wire_len);
    TEST_ASSERT_EQUAL_HEX8(0xAB, tampered[0]); /* map de 11 éléments */
    tampered[0] = 0xAA;                         /* map de 10 -> rejet immédiat */
    TEST_ASSERT_NOT_EQUAL(ESP_OK,
                          meshpay_currency_descriptor_decode(tampered, wire_len,
                                                             &decoded));

    /* (d) Longueur du bstr founder trafiquée (64 -> 65). */
    memcpy(tampered, wire, wire_len);
    TEST_ASSERT_EQUAL_HEX8(0x58, tampered[2]); /* bstr, longueur sur 1 octet */
    TEST_ASSERT_EQUAL_HEX8(0x40, tampered[3]); /* = 64 */
    tampered[3] = 0x41;                          /* annonce 65, attendu 64 -> rejet */
    TEST_ASSERT_NOT_EQUAL(ESP_OK,
                          meshpay_currency_descriptor_decode(tampered, wire_len,
                                                             &decoded));

    /* (e) Clé dupliquée + clé manquante : on remplace la clé de la signature
     *     (11) par celle du founder (1). Le founder apparaît deux fois et la
     *     signature manque -> contrôle des clés requises -> rejet. */
    memcpy(tampered, wire, wire_len);
    TEST_ASSERT_EQUAL_HEX8(0x0B, tampered[wire_len - 67]); /* clé signature = 11 */
    tampered[wire_len - 67] = 0x01;                         /* -> clé founder = 1 */
    TEST_ASSERT_NOT_EQUAL(ESP_OK,
                          meshpay_currency_descriptor_decode(tampered, wire_len,
                                                             &decoded));
}

/*
 * Substitution d'autorité : c'est le CŒUR du modèle de sécurité (le fondateur
 * est la seule autorité de frappe). Aucun attaquant ne doit pouvoir détourner
 * un descripteur pour se faire passer pour l'autorité d'une monnaie existante.
 */
TEST_CASE("currency descriptor substitution d'autorite rejetee", "[currency_descriptor]")
{
    rns_identity_t founder_a;
    rns_identity_t founder_b;
    TEST_ASSERT_EQUAL(ESP_OK, rns_identity_generate(&founder_a));
    TEST_ASSERT_EQUAL(ESP_OK, rns_identity_generate(&founder_b));

    meshpay_currency_descriptor_t body;
    fill_body(&body);

    meshpay_currency_descriptor_signed_t desc_a;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_currency_descriptor_sign(&desc_a, &body,
                                                               &founder_a));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_currency_descriptor_verify(&desc_a));

    /* Attaque 1 : remplacer la clé publique du fondateur par celle de B sans
     * re-signer. Le genesis recalculé (sur la clé de B) diffère du genesis
     * stocké (sur la clé de A) -> rejet. */
    meshpay_currency_descriptor_signed_t attack1 = desc_a;
    uint8_t pub_b[RNS_IDENTITY_PUBLIC_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK, rns_identity_get_public_key(&founder_b, pub_b));
    memcpy(attack1.body.founder_public, pub_b, sizeof(pub_b));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      meshpay_currency_descriptor_verify(&attack1));

    /* Attaque 2 : garder le corps de A (genesis valide) mais remplacer la
     * signature par celle de B sur le même genesis. La signature ne vérifie
     * plus contre la clé de A embarquée -> rejet. */
    meshpay_currency_descriptor_signed_t attack2 = desc_a;
    TEST_ASSERT_EQUAL(ESP_OK, rns_identity_sign(&founder_b, attack2.genesis_hash,
                                                sizeof(attack2.genesis_hash),
                                                attack2.founder_signature));
    TEST_ASSERT_NOT_EQUAL(ESP_OK,
                          meshpay_currency_descriptor_verify(&attack2));

    /* Contraste : B peut créer SA PROPRE monnaie (même corps, autre fondateur)
     * -> descripteur valide, mais genesis et currency_id DIFFÉRENTS de A (ce
     * n'est pas la monnaie de A). */
    meshpay_currency_descriptor_signed_t desc_b;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_currency_descriptor_sign(&desc_b, &body,
                                                               &founder_b));
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_currency_descriptor_verify(&desc_b));
    TEST_ASSERT_NOT_EQUAL(desc_a.currency_id, desc_b.currency_id);
    TEST_ASSERT_NOT_EQUAL(0, memcmp(desc_a.genesis_hash, desc_b.genesis_hash,
                                    sizeof(desc_a.genesis_hash)));
}

TEST_CASE("currency descriptor encode rejette un buffer trop petit", "[currency_descriptor]")
{
    rns_identity_t founder;
    TEST_ASSERT_EQUAL(ESP_OK, rns_identity_generate(&founder));
    meshpay_currency_descriptor_t body;
    fill_body(&body);
    meshpay_currency_descriptor_signed_t signed_desc;
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_currency_descriptor_sign(&signed_desc, &body,
                                                               &founder));

    /* Buffers notoirement insuffisants : refus propre, pas de débordement. */
    uint8_t tiny[8];
    size_t out_len = 0;
    TEST_ASSERT_NOT_EQUAL(ESP_OK,
                          meshpay_currency_descriptor_encode(&signed_desc, tiny,
                                                             sizeof(tiny), &out_len));
    uint8_t tiny_body[4];
    TEST_ASSERT_NOT_EQUAL(ESP_OK,
                          meshpay_currency_descriptor_encode_body(&body, tiny_body,
                                                                  sizeof(tiny_body),
                                                                  &out_len));
}

TEST_CASE("currency descriptor rejette les arguments NULL", "[currency_descriptor]")
{
    meshpay_currency_descriptor_t body;
    fill_body(&body);
    meshpay_currency_descriptor_signed_t signed_desc;
    uint8_t buf[MESHPAY_CURRENCY_DESCRIPTOR_CBOR_MAX];
    size_t len = 0;
    uint8_t hash[RNS_IDENTITY_HASH_SIZE];
    uint8_t genesis[MESHPAY_CURRENCY_GENESIS_SIZE];

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      meshpay_currency_descriptor_encode_body(NULL, buf, sizeof(buf),
                                                              &len));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      meshpay_currency_descriptor_compute_genesis(NULL, genesis, NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      meshpay_currency_descriptor_sign(NULL, &body, NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      meshpay_currency_descriptor_verify(NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      meshpay_currency_descriptor_encode(NULL, buf, sizeof(buf),
                                                         &len));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      meshpay_currency_descriptor_decode(NULL, 0, &signed_desc));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      meshpay_currency_descriptor_founder_hash(NULL, hash));
}

/* ======================================================================== */
/* Palier B1 — code d'invitation (ancre base32 Crockford + checksum)        */
/* ======================================================================== */

/*
 * Forge un descripteur signé « réaliste » et renvoie son ancre théorique
 * (= les MESHPAY_CURRENCY_INVITE_ANCHOR_LEN premiers octets du genesis). Mutualisé
 * par les tests d'invitation pour éviter la répétition de sign().
 */
static void make_signed(meshpay_currency_descriptor_signed_t *signed_desc)
{
    rns_identity_t founder;
    TEST_ASSERT_EQUAL(ESP_OK, rns_identity_generate(&founder));
    meshpay_currency_descriptor_t body;
    fill_body(&body);
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_currency_descriptor_sign(signed_desc, &body, &founder));
}

/* Vrai ssi c est un symbole de l'alphabet Crockford (majuscule canonique). */
static bool is_crockford_upper(char c)
{
    static const char *ALPHA = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
    return strchr(ALPHA, c) != NULL && c != '\0';
}

TEST_CASE("currency invite encode produit le format attendu", "[currency_descriptor]")
{
    meshpay_currency_descriptor_signed_t signed_desc;
    make_signed(&signed_desc);

    char code[MESHPAY_CURRENCY_INVITE_CODE_BUF];
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_currency_invite_encode(&signed_desc, code, sizeof(code)));

    /* Format groupé 4-4-4-4-2 : 18 symboles + 4 tirets = 22 caractères. */
    TEST_ASSERT_EQUAL_size_t(22, strlen(code));
    TEST_ASSERT_EQUAL_CHAR('-', code[4]);
    TEST_ASSERT_EQUAL_CHAR('-', code[9]);
    TEST_ASSERT_EQUAL_CHAR('-', code[14]);
    TEST_ASSERT_EQUAL_CHAR('-', code[19]);
    /* Tout caractère non-tiret est un symbole Crockford majuscule. */
    for (size_t i = 0; code[i] != '\0'; ++i) {
        if (code[i] == '-') {
            continue;
        }
        TEST_ASSERT_TRUE(is_crockford_upper(code[i]));
    }
}

TEST_CASE("currency invite round-trip restitue l'ancre", "[currency_descriptor]")
{
    meshpay_currency_descriptor_signed_t signed_desc;
    make_signed(&signed_desc);

    char code[MESHPAY_CURRENCY_INVITE_CODE_BUF];
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_currency_invite_encode(&signed_desc, code, sizeof(code)));

    uint8_t anchor[MESHPAY_CURRENCY_INVITE_ANCHOR_LEN];
    size_t anchor_len = 0;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_currency_invite_decode(code, anchor, sizeof(anchor),
                                                     &anchor_len));
    /* L'ancre décodée = les 10 octets de tête du genesis. */
    TEST_ASSERT_EQUAL_size_t(MESHPAY_CURRENCY_INVITE_ANCHOR_LEN, anchor_len);
    TEST_ASSERT_EQUAL_MEMORY(signed_desc.genesis_hash, anchor,
                             MESHPAY_CURRENCY_INVITE_ANCHOR_LEN);
}

TEST_CASE("currency invite checksum KO rejete", "[currency_descriptor]")
{
    meshpay_currency_descriptor_signed_t signed_desc;
    make_signed(&signed_desc);

    char code[MESHPAY_CURRENCY_INVITE_CODE_BUF];
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_currency_invite_encode(&signed_desc, code, sizeof(code)));

    /* Corrompre le symbole qui porte l'octet de CHECKSUM (et pas l'ancre) rend le
     * rejet DÉTERMINISTE : l'ancre décodée reste identique, le checksum recalculé
     * dessus reste donc le bon, mais le checksum stocké lu est faux → mismatch
     * garanti (vs. flipper un symbole de données qui ne casse le checksum qu'à
     * 255/256). Disposition : 18 symboles 5 bits ; l'ancre = bits 0..79 = symboles
     * 0..15 ; l'octet de checksum = bits 80..87 = symbole 16 (+ une partie du 17).
     * Le symbole 16 est le 1er du dernier groupe « -XX », soit l'index 20 de la
     * chaîne formatée (après le 4e tiret en position 19). */
    code[20] = (code[20] == 'A') ? 'B' : 'A';

    uint8_t anchor[MESHPAY_CURRENCY_INVITE_ANCHOR_LEN];
    size_t anchor_len = 0;
    TEST_ASSERT_NOT_EQUAL(ESP_OK,
                          meshpay_currency_invite_decode(code, anchor, sizeof(anchor),
                                                         &anchor_len));
}

TEST_CASE("currency invite caractere hors alphabet rejete", "[currency_descriptor]")
{
    meshpay_currency_descriptor_signed_t signed_desc;
    make_signed(&signed_desc);

    char code[MESHPAY_CURRENCY_INVITE_CODE_BUF];
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_currency_invite_encode(&signed_desc, code, sizeof(code)));

    /* Injecter un caractère hors alphabet (et non-tiret). */
    code[0] = '@';

    uint8_t anchor[MESHPAY_CURRENCY_INVITE_ANCHOR_LEN];
    size_t anchor_len = 0;
    TEST_ASSERT_NOT_EQUAL(ESP_OK,
                          meshpay_currency_invite_decode(code, anchor, sizeof(anchor),
                                                         &anchor_len));
}

TEST_CASE("currency invite longueur invalide rejetee", "[currency_descriptor]")
{
    uint8_t anchor[MESHPAY_CURRENCY_INVITE_ANCHOR_LEN];
    size_t anchor_len = 0;

    /* Trop court. */
    TEST_ASSERT_NOT_EQUAL(ESP_OK,
                          meshpay_currency_invite_decode("ABCD-EFGH",
                                                         anchor, sizeof(anchor),
                                                         &anchor_len));
    /* Trop long (19 symboles utiles). */
    TEST_ASSERT_NOT_EQUAL(ESP_OK,
                          meshpay_currency_invite_decode(
                              "ABCDEFGHJKMNPQRSTVW", anchor, sizeof(anchor),
                              &anchor_len));
    /* Chaîne vide. */
    TEST_ASSERT_NOT_EQUAL(ESP_OK,
                          meshpay_currency_invite_decode("", anchor, sizeof(anchor),
                                                         &anchor_len));
}

TEST_CASE("currency invite normalisation Crockford et tirets optionnels", "[currency_descriptor]")
{
    meshpay_currency_descriptor_signed_t signed_desc;
    make_signed(&signed_desc);

    char code[MESHPAY_CURRENCY_INVITE_CODE_BUF];
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_currency_invite_encode(&signed_desc, code, sizeof(code)));

    /* Variante saisie « à la main » : minuscules, sans tirets, avec O/I/L à la
     * place de 0/1 — Crockford doit normaliser et décoder à l'identique. */
    char variant[MESHPAY_CURRENCY_INVITE_CODE_BUF];
    size_t j = 0;
    for (size_t i = 0; code[i] != '\0'; ++i) {
        char c = code[i];
        if (c == '-') {
            continue; /* tirets retirés */
        }
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a'); /* minuscule */
        }
        if (c == '0') {
            c = 'O'; /* sosie : doit remapper vers 0 */
        } else if (c == '1') {
            c = 'l'; /* sosie : doit remapper vers 1 */
        }
        variant[j++] = c;
    }
    variant[j] = '\0';

    uint8_t a_ref[MESHPAY_CURRENCY_INVITE_ANCHOR_LEN];
    uint8_t a_var[MESHPAY_CURRENCY_INVITE_ANCHOR_LEN];
    size_t l_ref = 0, l_var = 0;
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_currency_invite_decode(code, a_ref, sizeof(a_ref), &l_ref));
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_currency_invite_decode(variant, a_var, sizeof(a_var), &l_var));
    TEST_ASSERT_EQUAL_size_t(l_ref, l_var);
    TEST_ASSERT_EQUAL_MEMORY(a_ref, a_var, l_ref);
}

TEST_CASE("currency invite NULL rejete", "[currency_descriptor]")
{
    meshpay_currency_descriptor_signed_t signed_desc;
    make_signed(&signed_desc);
    char code[MESHPAY_CURRENCY_INVITE_CODE_BUF];
    uint8_t anchor[MESHPAY_CURRENCY_INVITE_ANCHOR_LEN];
    size_t anchor_len = 0;

    /* Encode : descripteur ou sortie NULL, ou buffer trop petit. */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      meshpay_currency_invite_encode(NULL, code, sizeof(code)));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      meshpay_currency_invite_encode(&signed_desc, NULL, sizeof(code)));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE,
                      meshpay_currency_invite_encode(&signed_desc, code, 4));

    /* Decode : code NULL, sortie NULL, ou capacité d'ancre insuffisante. */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      meshpay_currency_invite_decode(NULL, anchor, sizeof(anchor),
                                                     &anchor_len));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      meshpay_currency_invite_decode("ABCD", NULL, sizeof(anchor),
                                                     &anchor_len));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE,
                      meshpay_currency_invite_decode("ABCD", anchor, 2, &anchor_len));
}

/* ======================================================================== */
/* Palier B2 — contrôle d'ancre (matches_anchor)                            */
/* ======================================================================== */

TEST_CASE("currency descriptor matches_anchor accepte le bon prefixe", "[currency_descriptor]")
{
    meshpay_currency_descriptor_signed_t signed_desc;
    make_signed(&signed_desc);

    /* L'ancre = les 10 octets de tête de la genèse. */
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_currency_descriptor_matches_anchor(
                          &signed_desc, signed_desc.genesis_hash,
                          MESHPAY_CURRENCY_INVITE_ANCHOR_LEN));
}

TEST_CASE("currency descriptor matches_anchor rejette un bit different", "[currency_descriptor]")
{
    meshpay_currency_descriptor_signed_t signed_desc;
    make_signed(&signed_desc);

    /* Copier l'ancre puis flipper 1 bit : le préfixe ne correspond plus. */
    uint8_t anchor[MESHPAY_CURRENCY_INVITE_ANCHOR_LEN];
    memcpy(anchor, signed_desc.genesis_hash, sizeof(anchor));
    anchor[3] ^= 0x01;

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      meshpay_currency_descriptor_matches_anchor(
                          &signed_desc, anchor, sizeof(anchor)));
}

TEST_CASE("currency descriptor matches_anchor gere les longueurs partielles", "[currency_descriptor]")
{
    meshpay_currency_descriptor_signed_t signed_desc;
    make_signed(&signed_desc);

    /* len=4 : les octets du currency_id doivent matcher. */
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_currency_descriptor_matches_anchor(
                          &signed_desc, signed_desc.genesis_hash, 4));
    /* len=32 : la genèse entière doit matcher. */
    TEST_ASSERT_EQUAL(ESP_OK,
                      meshpay_currency_descriptor_matches_anchor(
                          &signed_desc, signed_desc.genesis_hash,
                          MESHPAY_CURRENCY_GENESIS_SIZE));
}

TEST_CASE("currency descriptor matches_anchor recalcule depuis le corps", "[currency_descriptor]")
{
    meshpay_currency_descriptor_signed_t signed_desc;
    make_signed(&signed_desc);

    /* Mémoriser l'ancre légitime AVANT trafic. */
    uint8_t anchor[MESHPAY_CURRENCY_INVITE_ANCHOR_LEN];
    memcpy(anchor, signed_desc.genesis_hash, sizeof(anchor));

    /* Trafiquer une règle du corps : la genèse recalculée change, donc l'ancre
     * d'origine ne doit plus matcher — même si genesis_hash stocké est inchangé.
     * (matches_anchor ne fait pas confiance au champ stocké.) */
    signed_desc.body.max_supply += 1;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      meshpay_currency_descriptor_matches_anchor(
                          &signed_desc, anchor, sizeof(anchor)));
}

TEST_CASE("currency descriptor matches_anchor rejette les arguments invalides", "[currency_descriptor]")
{
    meshpay_currency_descriptor_signed_t signed_desc;
    make_signed(&signed_desc);
    uint8_t anchor[MESHPAY_CURRENCY_INVITE_ANCHOR_LEN] = {0};

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      meshpay_currency_descriptor_matches_anchor(
                          NULL, anchor, sizeof(anchor)));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      meshpay_currency_descriptor_matches_anchor(
                          &signed_desc, NULL, sizeof(anchor)));
    /* len == 0 : rien à comparer, argument invalide. */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      meshpay_currency_descriptor_matches_anchor(
                          &signed_desc, anchor, 0));
    /* len > taille de la genèse : impossible. */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      meshpay_currency_descriptor_matches_anchor(
                          &signed_desc, anchor,
                          MESHPAY_CURRENCY_GENESIS_SIZE + 1));
}
