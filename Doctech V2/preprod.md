# Préproduction MeshPayV2

## État

Le socle fonctionnel est atteint : les 24 blocs du plan ont un premier niveau d'implémentation avec tests Unity, et les Waveshare S3 Touch bootent, affichent, découvrent les pairs et exécutent des paiements sur banc.

Le projet reste toutefois en phase de durcissement préproduction. Le prochain travail n'est pas de refaire l'architecture, mais de stabiliser les chemins réels : persistance, sécurité, radio, UX d'erreur et procédure de release.

## Priorité 0

- Stabiliser la découverte à 3 devices : cold boot des 3, arrivée tardive, reboot d'un seul, compteur peers attendu `2` partout.
- Valider les paiements répétés : A vers B, B vers C, C vers A, plusieurs fois, avec soldes corrects après chaque ACK.
- Rendre la DAG vraiment durable : finaliser les checkpoints automatiques et la restauration complète après reboot ou power loss.
- Tester les coupures pendant un paiement pending : ne pas perdre la séquence, ne pas double-dépenser, ne pas bloquer le wallet.
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
