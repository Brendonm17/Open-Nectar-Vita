#pragma once

// Gestor de crash de Pikmin 1, como el de Pikmin 2 (pikmin2/pc_port/pc_main.cpp):
// escribe pikmin_crash.log junto a los datos (versión, commit, plataforma y
// backtrace) y, en PC, abre el launcher con --crash-report para que ofrezca
// copiarlo y abrir un issue de GitHub. Sin terminal, la salida del juego va a
// pikmin.log, que el launcher añade al informe.
// Llamar al principio de main, con el directorio de trabajo ya en la carpeta
// del juego (en Android, tras pc_android_init).
void pc_install_crash_handler();
