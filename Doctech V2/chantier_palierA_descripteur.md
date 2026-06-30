# Spec + plan — Palier A : Descripteur de monnaie signé + persistance + durcissement MINT

> Statut : **plan validé, en implémentation** · Créé le 2026-06-30
> Deuxième palier du chantier global **« Création de monnaie & rejointe de réseau »**
> (cf. `chantier_palier0_bringup_tdeck.md` §0 pour les décisions verrouillées).
> Logique **pure** : entièrement testable sans matériel (TDD Unity).

---

## 0. Rappel des décisions verrouillées (brainstorm 2026-06-29)

- **Gouvernance** : émetteur **unique (fondateur)** — seule autorité MINT.
- **Rejointe** : code d'invitation au pavé ancrant un **descripteur de monnaie signé**
  diffusé par radio (Palier B) + voucher hors-ligne (Palier C).
- **Portée** : **mono-monnaie** par device.
- **Règles v1** : nom/symbole, frais + plafond (supply), fonte (demurrage), crédit initial.
- **Fondateur = T-Deck Plus**, membres = wallets existants.

---

## 1. Correction de cap — il n'y a PAS de self-mint à retirer

La spec initiale parlait de « retirer le self-mint / boot credit ». **L'audit du code montre
qu'il n'en existe aucun** :

- `meshpay_currency_validate_tx` (`currency.c:153`) n'accepte un MINT que si
  `meshpay_currency_is_mint_authority(config, tx->from)` est vrai. Un wallet démarre à 0.

Le **vrai** durcissement est ailleurs, et plus sérieux :

> **Faille** : `tx->from` est un **hash public** (16 o). Le MINT vérifie l'appartenance de
> `from` aux autorités, mais **la signature de la TX n'est vérifiée qu'au niveau
> `payment_engine`, et seulement « si l'identité émettrice est connue »**
> (`verify_sender_if_known`). Un membre qui n'a jamais vu l'identité du fondateur
> accepterait une **fausse TX MINT** (`from` = hash fondateur, public ; signature
> arbitraire) → **inflation frauduleuse**.

**Le descripteur ferme la faille** : il transporte la **clé publique du fondateur**, ce qui
permet de **vérifier la signature de toute TX MINT de façon inconditionnelle**, même sans
connaître l'identité émettrice par ailleurs.

---

## 2. Contrainte transverse — radio-agnostique dès maintenant

Même si la **diffusion** du descripteur est le Palier B, son **format wire est figé en
Palier A** et doit être transport-agnostique :

- sérialisation **CBOR compacte** (même style que `meshpay_tx`), **bornée** ;
- taille cible **≤ ~280 o** → tient dans le **MTU RNS 500** et se fragmente proprement sur
  **LoRa (fragments 255 o → 2 fragments max)** ; passe aussi par **ESP-NOW** ;
- **aucune dépendance au bearer** dans la structure : le descripteur transite indifféremment
  par `rns_iface_espnow` ou `rns_iface_lora` via la pile `rns_*` (déjà abstraite).
- On ne **teste** qu'en ESP-NOW pour l'instant, mais le format ne présuppose aucun bearer.

---

## 3. Modèle de données

### 3.1 Corps signable (les règles + l'autorité)

```c
#define MESHPAY_CURRENCY_NAME_MAX   24   /* nom monnaie, null-term */
#define MESHPAY_CURRENCY_SYMBOL_MAX  8   /* symbole, null-term */

typedef struct {
    uint8_t  founder_public[RNS_IDENTITY_PUBLIC_SIZE]; /* 64 — X25519||Ed25519 pub fondateur */
    char     name[MESHPAY_CURRENCY_NAME_MAX];          /* nom de la monnaie */
    char     symbol[MESHPAY_CURRENCY_SYMBOL_MAX];      /* symbole */
    uint64_t max_supply;                               /* plafond (0 = illimité) */
    uint32_t transfer_fee;                             /* frais de transfert */
    bool     demurrage_enabled;                        /* fonte activée ? */
    uint16_t demurrage_bps;                            /* taux de fonte (bps) */
    uint32_t initial_credit;                           /* crédit initial membre (règle v1) */
    uint64_t created_at_ms;                            /* horodatage de création */
} meshpay_currency_descriptor_t;
```

### 3.2 Descripteur signé (wire + persistance)

```c
typedef struct {
    meshpay_currency_descriptor_t body;
    uint32_t currency_id;                                          /* DÉRIVÉ : genesis_hash[0..4] BE */
    uint8_t  genesis_hash[RNS_CRYPTO_SHA256_SIZE];                 /* 32 — SHA-256(CBOR canonique du body) */
    uint8_t  founder_signature[RNS_CRYPTO_ED25519_SIGNATURE_SIZE]; /* 64 — sig fondateur sur genesis_hash */
} meshpay_currency_descriptor_signed_t;
```

**Identité de la monnaie, déterministe et auto-cohérente :**
- `genesis_hash = SHA-256( encode_body(body) )` — encodage CBOR **canonique** (clés triées).
- `currency_id  = genesis_hash[0..3]` en big-endian (uint32) — **dérivé**, jamais transmis
  séparément ni inclus dans le corps haché (pas de circularité).
- Changer **une seule règle** ⇒ genesis_hash différent ⇒ **autre monnaie**. Les TX portent
  `currency_id` ; `validate_tx` rejette déjà les mauvais `currency_id`.
- L'**autorité MINT** = `rns_identity_get_hash(founder_public)` (16 o). Le fondateur est la
  **seule** autorité.

---

## 4. Découpage TDD (A1 → A5)

| Étape | Contenu | Dépend de | Délégable |
|---|---|---|---|
| **A1** | Composant **`currency_descriptor/`** : struct + CBOR `encode_body`/`encode`/`decode` + `compute_genesis` + `sign` + `verify` + `founder_hash`. Tests Unity (round-trip, genesis déterministe, sig valide/invalide, taille bornée). | — | oui (gros) |
| **A2** | **Pont** `meshpay_currency_config_from_descriptor()` : remplit la config runtime (currency_id, supply, fee, demurrage) + pose le hash fondateur comme **unique** autorité MINT + stocke `founder_public`. Étend `meshpay_currency_config_t` (`founder_public[64]`, `has_descriptor`). | A1 | moyen |
| **A3** | **Durcissement** `meshpay_currency_validate_tx` (MINT) : si `has_descriptor`, **vérifier inconditionnellement la signature** de la TX contre `founder_public` (via `meshpay_tx_verify` sur une identité publique reconstruite). Test clé : MINT bien formé mais **mauvaise signature ⇒ rejet**. | A2 | moyen |
| **A4** | **Persistance** : `meshpay_storage_record_t` v2 — stocke le **blob CBOR** du descripteur (`uint8_t[]` + `len` + `has_currency_descriptor`) ; storage reste **agnostique** (pas de dépendance à `currency_descriptor`). Setter + load + erase. Tests round-trip NVS mock + rétro-compat v1. | A1 | moyen |
| **A5** | **Câblage boot** (`app_main_logic`) : au démarrage, charger le blob depuis storage → décoder → **vérifier** → dériver la config monnaie. Absent ⇒ device **vierge** (pas de monnaie). Tests `app_main_logic`. | A2, A3, A4 | à évaluer (touche le boot) |

**Parallélisable** : après A1, **A4** (persistance, ne dépend que des types) peut partir en
parallèle de **A2**. **A3** suit A2. **A5** clôt.

---

## 5. Tests (CLAUDE.md : un test unitaire par fonction, aucun TEST_IGNORE)

- **A1** : encode/decode round-trip ; `genesis_hash` déterministe (mêmes règles ⇒ même hash ;
  règle modifiée ⇒ hash différent) ; `currency_id` = 4 octets de tête ; signature valide
  acceptée / corrompue rejetée / corps trafiqué rejeté ; **taille encodée ≤ borne MTU**.
- **A2** : config dérivée correcte ; autorité unique = hash fondateur ; idempotence.
- **A3** : MINT signé par le fondateur **accepté** ; MINT `from`=fondateur mais **signé par un
  autre ⇒ `ERR_NOT_AUTHORITY`/`ERR_INVALID`** (reproduit la faille) ; TRANSFER inchangé.
- **A4** : save/load blob ; record v1 sans descripteur toujours lisible (`has_*`=false).
- **A5** : boot avec descripteur valide → config monnaie chargée ; descripteur corrompu →
  refus + device vierge ; absence → device vierge.

---

## 6. Hors-périmètre (paliers suivants)

- **Diffusion** du descripteur par radio + **code d'invitation** + adoption membre → **Palier B**.
- **Vouchers / crédit initial** effectivement crédité → **Palier C**
  (s'appuiera sur le conflit DAG `voucher_nonce` ; survie au checkpoint élagueur Phase B).
- **Wizard fondateur** (saisie nom/règles au clavier) + écran rejointe → **Palier D**.

---

## 7. Risques / points de vigilance

1. **Dépendances CMake** : `currency` devra dépendre de `rns_identity`/`meshpay_tx` pour la
   vérif signature (A3) ; `currency_descriptor` de `rns_crypto`/`rns_identity`. Garder
   `storage` **agnostique** (blob opaque) pour ne pas créer de cycle.
2. **CBOR canonique** : l'ordre des clés doit être **stable** entre `sign` et `verify`
   (sinon genesis_hash diverge). Encodage déterministe = clés croissantes.
3. **Rétro-compat persistance** : bump `version` du record + chemin de lecture v1 tolérant
   (cf. piège mémoire « NVS survit au flash chiffré »).
4. **Taille wire** : vérifier au test que l'encodé reste ≤ ~280 o (marge MTU/LoRa).
5. **A5 touche le boot** : prudence, ne pas casser le flux wallet existant ; device sans
   descripteur doit booter normalement (rôle vierge).
