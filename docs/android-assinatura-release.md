# Assinatura do APK Android: debug, release e compatibilidade de atualização

## Como está hoje

- O APK publicado pelo `android.yml` (releases `android-4.0-build-N`) é o build **debug**, assinado com
  `Android/app/debug.keystore`, que está no repositório (senha `android`). Foi colocado lá para que todo build
  do CI tivesse a mesma assinatura e instalasse por cima do anterior.
- Essa chave é pública: qualquer pessoa pode assinar um APK com ela, e o Android aceitaria esse APK como
  atualização do jogo instalado. Um build debug também é `debuggable`.
- Este repositório **não muda** a assinatura do APK distribuído. Quem tem o app instalado continua recebendo
  atualizações normalmente.

## O que já existe a partir desta versão

- `assembleRelease` gera um build release com `debuggable false` e `jniDebuggable false`.
- A assinatura release vem de fora do repositório:
  - no CI, dos GitHub Secrets `MMADE_RELEASE_KEYSTORE_BASE64`, `MMADE_RELEASE_STORE_PASSWORD`,
    `MMADE_RELEASE_KEY_ALIAS` e `MMADE_RELEASE_KEY_PASSWORD`;
  - numa máquina de desenvolvimento, de `~/.gradle/gradle.properties` (`mmade.release.storeFile`,
    `mmade.release.storePassword`, `mmade.release.keyAlias`, `mmade.release.keyPassword`) ou das variáveis
    `MMADE_RELEASE_*`.
- Sem esses valores, `assembleRelease` gera `app-release-unsigned.apk` e nada falha.
- O job `apk-release` do `android.yml` compila o release em todo push e PR e guarda o APK como artefato do
  workflow. Ele **não** é publicado como release enquanto a troca de assinatura abaixo não for decidida.
- O build debug continua igual, com a chave do repositório, para desenvolvimento e para a distribuição atual.

## Por que não trocar a chave de uma vez

O Android só instala uma atualização assinada pela mesma chave (ou por uma chave que a anterior autorizou).
Um APK assinado com uma chave nova sobre o app atual dá "conflito com um pacote já existente", e a pessoa
precisa desinstalar. Desinstalar apaga as preferências do app (opções da abertura etc.), mas **não** a pasta
`Majoras Mask Android Definitive Edition` (saves, mods, traduções, músicas e o `mm.o2r`), que fica no
armazenamento compartilhado. Desde esta versão, a versão do `mm.o2r` também fica gravada nessa pasta, então uma
reinstalação não obriga a recriar o `mm.o2r`.

## Plano de troca (precisa da decisão do Rafael)

### Opção recomendada: chave nova com rotação (APK Signature Scheme v3)

Em Android 9 ou mais novo, a chave antiga pode autorizar a nova. O app instalado aceita a atualização sem
desinstalar, e depois disso só aceita APKs da chave nova. Em Android 7 e 8 a rotação não existe: nesses aparelhos é
preciso desinstalar uma vez (os dados da pasta ficam).

1. Criar a chave release (uma vez, fora do repositório, com backup seguro):
   `keytool -genkeypair -v -keystore mmade-release.jks -alias mmade -keyalg RSA -keysize 4096 -validity 36500`
2. Criar a linhagem em que a chave debug autoriza a nova, e tirar da chave debug os direitos depois da troca:
   `apksigner rotate --out mmade-lineage.bin --old-signer --ks Android/app/debug.keystore --ks-key-alias androiddebugkey --ks-pass pass:android --new-signer --ks mmade-release.jks --ks-key-alias mmade`
   e depois
   `apksigner lineage --in mmade-lineage.bin --out mmade-lineage.bin --signer --ks Android/app/debug.keystore --ks-key-alias androiddebugkey --ks-pass pass:android --set-installed-data false --set-shared-uid false --set-permission false --set-rollback false`
   (conferir com `apksigner lineage --in mmade-lineage.bin --print-certs`).
3. Guardar `mmade-release.jks` e `mmade-lineage.bin` em base64 e as senhas como GitHub Secrets.
4. No CI, assinar o APK release com `apksigner sign --lineage mmade-lineage.bin` (com as duas chaves) e
   `--rotation-min-sdk-version 28`, porque o Gradle não aplica linhagem sozinho. Conferir com
   `apksigner verify -v --print-certs`.
5. Publicar esse APK no lugar do debug, avisando nas notas que Android 7/8 precisa desinstalar uma vez.

### Alternativas

- **Chave nova sem rotação:** mais simples, mas todo mundo precisa desinstalar uma vez.
- **Continuar como está:** nenhuma mudança para quem usa, mas a chave continua pública.

Nada disso é feito automaticamente: os secrets só podem ser criados pelo dono do repositório, e a troca da
distribuição só acontece depois da decisão.
