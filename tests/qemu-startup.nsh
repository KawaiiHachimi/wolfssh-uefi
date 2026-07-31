@echo -off
fs0:\wolfssh.efi -f @HOST_KEY_FINGERPRINT@ -P test test@10.0.2.2 -p 2222
reset -s
