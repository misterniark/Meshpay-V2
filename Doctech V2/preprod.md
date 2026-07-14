# Préproduction MeshPayV2

## État

Le socle fonctionnel est atteint : les 24 blocs du plan ont un premier niveau d'implémentation avec tests Unity, et les Waveshare S3 Touch bootent, affichent, découvrent les pairs et exécutent des paiements sur banc.

Le projet reste toutefois en phase de durcissement préproduction. Le prochain travail n'est pas de refaire l'architecture, mais de stabiliser les chemins réels : persistance, sécurité, radio, UX d'erreur et procédure de release.

## Priorité 0

- Stabiliser la découverte à 3 devices : cold boot des 3, arrivée tardive, reboot d'un seul, compteur peers attendu `2` partout.
- Valider les paiements répétés : A vers B, B vers C, C vers A, plusieurs fois, avec soldes corrects après chaque ACK.
- Rendre la DAG vraiment durable : ✅ Phase A faite (persistance + restauration au boot, composant `dag_store`) ; reste **Phase B = checkpoint élagueur > 200 TX** → prochain chantier détaillé dans `Doctech V2/chantier_phase_b_checkpoint.md` (touche le consensus, bloqué côté validation HW par l'injecteur §4.1).
- Tester les coupures pendant un paiement pending : ne pas perdre la séquence, ne pas double-dépenser, ne pas bloquer le wallet.
- **Validation à l'ingestion de la sync DAG (SÉCURITÉ, découvert à la revue du Palier C, 2026-07-14).** Le chemin `dag_sync` batch (`meshpay_dag_sync_apply_batch` → `meshpay_tx_decode` + `meshpay_dag_merge_tx`) merge les tx reçues **sans** `meshpay_tx_verify` (signature Ed25519) ni `meshpay_currency_validate_tx` (règles éco). Vrai pour **tous** les types (TRANSFER/MINT/CLAIM) depuis toujours — pas une régression du Palier C, mais un lien manquant du modèle de confiance : un pair peut forger et injecter des tx non signées. Défenses en aval partielles : MINT gaté par `is_mint_authority`, CLAIM gatée par `amount==initial_credit` (défense en profondeur ajoutée au Palier C), TRANSFER par la double entrée — mais **aucune ne remplace la vérif de signature**. Correctif = valider signature + règles éco à l'`apply_batch` ; **touche le consensus** (la reconciliation multi-passes ordre-tolérante ne peut pas rejeter une tx dont le parent n'est pas encore appliqué, et la vérif de signature exige la résolution d'identité du `from`). À traiter avec la **Phase B checkpoint/consensus** → chantier `Doctech V2/chantier_durcissement_ingestion.md`.
- Corriger tout écart découvert sur matériel avant d'ajouter de nouvelles features.

## Sécurité Production

- Passer du profil flash encryption `DEVELOPMENT` à une vraie politique production.
- Activer et valider Secure Boot, anti-rollback, désactivation JTAG/console selon besoin.
- Revoir les logs : ne jamais exposer de données sensibles et réduire le bruit série.
- Décider la politique PIN finale : chiffres visibles pour l'UX actuelle, ou mode optionnel masqué en production.
- Faire une revue crypto/licence : Monocypher, mbedTLS, compatibilité Reticulum, approche clean-room.

## Radio Et Réseau

- ESP-NOW : ajouter métriques, retry/backoff plus propres, gestion overflow queue, perte ACK et bruit radio.
- LoRa : mener une vraie campagne hardware. Le chemin production validé aujourd'hui est surtout Waveshare + ESP-NOW.
- Interop Reticulum : tester contre un noeud Reticulum Python réel sur announce, data chiffrée et resource.
- Rejointe monnaie (Palier B) : pendant qu'un device est « armé » (ancre posée), chaque OFFER `0x34` PLAIN broadcast déclenche un décodage CBOR + genèse (SHA-256) et, si l'ancre matche, une vérification Ed25519. Un pair à portée connaissant l'ancre (code d'invitation public) mais forgeant la signature peut donc flooder des OFFER non-matchants et forcer des vérifs asymétriques + un log par paquet. Impact borné (fenêtre courte, import réussi la referme, `matches_anchor` filtre avant l'Ed25519), mais à durcir au banc si observé : throttle (n OFFER traités / fenêtre) et/ou compteur de rejets. `matches_anchor` reste évalué AVANT `verify` (bon ordre : filtre bon marché avant la vérif coûteuse).

## Produit

- Définir les règles monétaires définitives : émission initiale, autorités `MINT`, frais, fonte/demurrage, résolution des conflits.
- Ajouter factory reset, reset PIN et effacement wallet.
- Améliorer l'UX petit écran : liste peers quand il y en a plus de deux, historique scrollable, erreurs radio claires, écran diagnostic simple.
- Ajouter l'état batterie/power si le hardware le permet.

## Release

- Mettre `MeshPayV2` sous Git proprement : le dossier courant n'apparaît pas encore comme repo Git.
- Ajouter une CI : `test_app`, fixtures `--check`, builds S3/CYD/H752/sécurisé.
- Versionner le firmware affiché/loggé et produire des artefacts release reproductibles.
- Écrire une procédure de flash production et une checklist QA.
