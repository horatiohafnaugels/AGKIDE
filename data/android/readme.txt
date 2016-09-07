This folder should contain the following files
 -a lib folder with the AGK interpreter libs
 -a sourceGoogle folder with the raw AndroidManifest.xml, res, resfacebook, and resgoogle folders, and the compiled classes.dex
 -a sourceAmazon folder with the raw AndroidManifest.xml, res, resfacebook, and resgoogle folders, and the compiled classes.dex
 -a sourceOuya folder with the raw AndroidManifest.xml, res, and the compiled classes.dex
The following are optional if you modify the paths in project.c:on_android_dialog_response() to point to a local Java installation and Android SDK installation
 -a copy of the JRE
 -aapt.exe
 -android23.jar
 -debug.keystore
 -zipalign.exe
