# Plan: niveles con la complejidad de los originales

Objetivo: que con el editor se pueda hacer un nivel como el Bosque de la
Esperanza, la Fuente Lejana o el Jardín Final: mismo tamaño, misma cantidad de
enemigos y objetos, y geometría libre (cuevas, troncos, salientes, alturas
encima de otras).

## Lo que tienen los originales (medido de los archivos del juego)

| Escenario | Vértices | Triángulos | Colisión | Tamaño aprox. |
|---|---|---|---|---|
| practice (Lugar del impacto) | 4.282 | 7.141 | 2.968 | 3.100 × 2.900 |
| stage1 (Bosque de la Esperanza) | 18.168 | 24.684 | 8.535 | 3.700 × 5.550 |
| stage2 (Ombligo del Bosque) | 15.350 | 26.017 | 5.163 | 4.300 × 5.200, 3.300 de alto |
| stage3 (Fuente Lejana) | 7.159 | 13.948 | 8.490 | 5.200 × 7.900 |
| laststage (Jardín Final) | 38.785 | 24.330 | 3.334 | 3.000 × 5.300 |

Un día del Bosque de la Esperanza junta unos 45 generadores de enemigos (cada
uno puede sacar varios), 6 de jefes, 42 plantas y unas 30 cosas entre puertas,
puentes, rocas y bombas. El motor reserva 80 enemigos (`TekiMgr::create(80)`).

Tipos de objeto que usan los .gen de la historia: `bomb`, `bomb gen`, `rockgen`,
`grassgen`, `kusa`, `weeds`, `sluice soft/hard/bomb/bombhard`, `bridge test`,
`move stone`, `goal`, `ufo` y las plantas `plnt` 0-11.

## Lo que tenemos ahora y dónde se queda corto

- **Geometría.** El terreno es un mapa de alturas (celda de 50): no admite
  techos, túneles, troncos huecos ni una altura encima de otra. Los props solo
  bloquean con su caja; no se pueden pisar.
- **Malla.** El tope es de 65.535 vértices, porque las listas de dibujo usan
  índices de 16 bits. Los originales caben, pero nuestra malla es poco
  eficiente: es una rejilla uniforme y duplica vértices por capa de material.
- **Límites del mapa.** 40 enemigos colocados, 24 construcciones, 16 pastillas,
  16 piezas y 4 guardianes. Salieron del VS y en parte son más bajos que lo que
  aguanta el motor.
- **Objetos que faltan.** La hierba que se arranca (`kusa`/`grassgen`), las
  malas hierbas (`weeds`), las plantas (`plnt`), las rocas bomba sueltas y sus
  generadores fuera del VS, y los puentes de varias longitudes con su trabajo.
- **Rutas.** Las rutas de acarreo se generan solas a partir de la rejilla; con
  geometría compleja hará falta poder editarlas.

## Fases

### Fase 15: límites al nivel del juego

**Hecho.**
- Límites compartidos (`kMapMax*` en `map.h` y `PC_VS_MAX_*` en
  `pc_vs_arena.h`, con `static_assert` para que coincidan): 80 enemigos y 40
  jefes (120 colocados), 64 construcciones, 48 pastillas, 32 piezas y 16
  guardianes.
- Motor (solo en el port):
  - montón de enemigos de 10 MB, como la DLL de Windows;
  - pools de objetos (192) y de pastillas (160) más grandes;
  - caché de mallas de 128 MB (64 MB en Android).
- Malla:
  - adiós al tope de 65.535: el compilador parte las mallas para que cada una
    quepa en índices de 16 bits y guarda su base en el bloque `0xF00`;
  - el juego la aplica en `DGXGraphics::setupVtxDesc` y `parseMod` la suma al
    leer;
  - los mapas que caben no cambian, así que los digests de netplay siguen
    iguales;
  - nuevo tope por memoria: 250.000 vértices y 300.000 triángulos.
- Editor: panel «Presupuesto del juego» con barras de vértices, enemigos,
  jefes, construcciones, pastillas, piezas y guardianes.
- Aplazado: simplificar la malla del terreno (quadtree). Ya no hace falta para
  caber; queda como optimización si el rendimiento en Android lo pide.
- Auditar el origen real de cada límite: mapa, `pc_vs_arena`, gestores del
  juego (`TekiMgr`, `BossMgr`, `ItemMgr`, `PlantMgr`) y montones de memoria del
  port.
- Subir los enemigos colocados a 80 (lo que reserva el juego). En el port de
  PC, subir `TekiMgr` y `BossMgr` (por ejemplo a 128 y 16) si la memoria lo
  permite, también en Android.
- Construcciones a 64, pastillas y piezas a 64, guardianes a 16 (siempre que
  sus gestores lo aguanten).
- Malla: quitar el tope de 65.535.
  - Opción A: partir el escenario en varias mallas, cada una con su propio
    rango de índices.
  - Opción B: índices de 32 bits en el cargador de display lists del port.
  - Hay que investigar cuál acepta `pc_gfx` sin tocar el render de los .mod
    originales.
- Simplificar la malla del terreno: unir las celdas llanas (quadtree) y no
  duplicar vértices por capa salvo en los bordes de material. Así cabe más
  mapa, o el mismo con celda de 25.
- Editor: panel de presupuesto (vértices, triángulos, colisión, enemigos,
  objetos y memoria) con barras frente al límite.

### Fase 16: los objetos del juego, con sus generadores

**Hecho.**
- **Catálogo.** «Objetos del juego (como en la historia)» en *Poner cosas*,
  con 58 clases sacadas de los `.gen` originales (`gameGenCatalog`):
  - plantas animadas;
  - hierba que se arranca, hierbajos, piedras, rocas bomba sueltas y sus
    montones;
  - néctar;
  - puertas de papel, de piedra, de roca y de roca negra dura;
  - puentes;
  - grupos de enemigos, jefes y géiseres;
  - Pikmin salvajes.

  Van al `default.gen` / `plants.gen` de la arena y los crea el juego con su
  propio código.
- **Ajustes por generador**, que solo reescriben los parámetros que ya tiene
  (`readGenSettings` / `writeGenSettings`; `GenParam::offset`):
  - radio del círculo (`p00`);
  - cuántos salen (`aton` `p00`, `irnd` `p00`/`p01`);
  - giro (`1one` `p01`);
  - días para volver a salir (`b00`).
- **VS.** «Repetir en el lado del Jugador 2» (`DocGen::mirror`): el gemelo se
  pone en espejo y girado 180°.
- **Límites.** Los enemigos y jefes de los generadores cuentan para los
  límites (`MapData::genTekis` / `genBosses`) y para el panel de presupuesto.
- **Misiones de varios días.** La caché de generadores del juego se guarda al
  anochecer en el guardado de la misión (`gcache`) y se carga al empezar el
  día. La de la historia se aparta y se devuelve, y la arena ya no usa la de
  la historia.
- **Fuera.** `move stone`, que guarda la posición de la roca en absoluto: para
  eso está el bloque del editor.
- **Pendiente.** Generadores solo de ciertos días (los `N.gen` / `A-B.gen` del
  juego); ahora todos salen cada día.
- En vez de crear cada objeto a mano (`pcCustomSpawnEntities`), compilar las
  entidades del mapa a un `.gen` de verdad (ya existe `writeGen`). Así el juego
  los crea con su propio código y vale cualquier tipo de la historia.
  Investigar si el arena virtual puede servir varios .gen (por día, como
  `0-29.gen` y `4-29.gen`).
- Objetos nuevos en el editor:
  - hierba de Pikmin (`kusa`, `grassgen`);
  - malas hierbas y piedrecitas (`weeds`);
  - plantas animadas (`plnt` 0-11, con vista previa);
  - rocas bomba sueltas y sus generadores (`bomb`, `bomb gen`, `rockgen`) en
    solo y coop;
  - la puerta `sluice bombhard`;
  - puentes con longitud y trabajo configurables (`bridge test`);
  - `move stone` con recorrido.
- Enemigos por generador: grupos en un área (N enemigos en un círculo), los
  días en que salen (ya está) y si se conservan de un día a otro
  (`carryOver`).

### Fase 17: geometría libre importada

**Hecho.**
- **Partir de un escenario original.** Ya existía: «O un escenario del juego»
  en la pantalla de inicio (`importCourse`). Usa el `.mod` del nivel tal
  cual, con sus generadores.
- **Importar OBJ** (*Poner cosas → Mallas propias (OBJ)*):
  - se lee el `.obj` con su `.mtl` (`Kd`, `d`, `map_Kd`) y texturas PNG o JPG;
  - las caras se convierten en triángulos y se giran al sentido del juego;
  - se guarda como `meshes/<nombre>.mod` en la carpeta del mapa (`writeMeshMod`);
  - las texturas pasan a RGB5A3 (potencia de 2, hasta 512), con material
    sencillo opaco, o con recorte si la textura tiene transparencias;
  - el objeto apunta a `map:meshes/<nombre>.mod`.
- **Carpeta del mapa.** El juego (`pc_map_catalog_load` → `MapData::dir`;
  `readGameFile`) y el editor (vista previa, modelos) leen los archivos
  `map:`. Los paquetes `.nectarpack` aceptan `.mod`.
- **Comprobación.** Al compilar, las texturas de cada objeto deben traer los
  datos que dicen sus lados (`modTextureBytes`).
- **«Se puede pisar»** (`DocProp::ground`):
  - su colisión es suelo y no corta los caminos;
  - las mallas propias lo traen activado;
  - en el juego, las cosas se colocan a la altura de la colisión si hay
    mallas así.
- **Pendiente:**
  - híbrido con escenarios del juego: en el modo escenario no se pueden añadir
    objetos ni mallas;
  - glTF;
  - mallas de más de 65.534 vértices (hay que partirlas en Blender);
  - los caminos de acarreo por encima de las mallas (fase 20).
- **Partir de un escenario original.** Elegir `courses/stage1/forest.mod`
  (etc.) como malla del nivel y poner encima nuestras entidades, misiones y
  disparadores. Con esto se pueden hacer niveles con exactamente la geometría
  original. Usa la copia del juego del propio usuario; no se distribuye nada.
- **Importar una malla** (OBJ y, después, glTF) hecha en Blender:
  - materiales a texturas: codificar PNG a los formatos de textura del .mod
    (RGB5A3 o CMPR), lo que obliga a escribir un codificador;
  - colisión sacada de la malla, con atributo por material (suelo, agua,
    pared, resbaladizo);
  - rejilla de colisión generada (`gridCell` 64, como los originales);
  - escala y orientación al importar, y aviso de triángulos degenerados.
- **Híbrido.** El terreno del editor más mallas importadas encima, con la
  colisión unida.

### Fase 18: piezas sólidas (kit de escenario)

**Hecho.**
- **Props con colisión real.** Ya chocaban con sus triángulos de verdad; solo
  cortaban caminos con su caja. Con «Se puede pisar» (fase 17) son suelo y no
  cortan caminos.
- **Recortes de escenarios** (`extractModRegion`):
  - se copian las mallas de una caja de un escenario con sus materiales, TEV y
    texturas originales, así que se ve igual que en el nivel;
  - se recoloca con el centro de la caja en planta y su suelo como origen;
  - el mapa solo guarda la referencia `cut:<.mod>|caja` y el recorte se saca
    al compilar de la copia del juego de cada uno, así que los paquetes no
    llevan datos del juego.
- **Editor:**
  - en el modo escenario, «Recortar una pieza de este escenario»: caja verde
    en la vista, centro, tamaño y alturas, y «Guardar en mi biblioteca»;
  - en cualquier mapa, «Mis piezas» (con miniatura renderizada) para ponerlas;
  - la biblioteca va en la configuración del editor (`pieces`).
- **Comprobado:** un recorte de 889 triángulos del Bosque de la Esperanza (19
  materiales, 12 texturas) compila dentro de un mapa y se ve con sus texturas.
- Props con colisión real: meter los triángulos de colisión del prop
  (movidos, girados y escalados) en la colisión del mapa, en vez de su caja.
  Así se pueden pisar troncos, puentes naturales y cajas.
- **Recortes.** Seleccionar una caja de un escenario original y guardarla como
  pieza con su malla, texturas y colisión (un tronco del Bosque, una lata o una
  raíz). Biblioteca de piezas en el editor.
- Esto da salientes, túneles y alturas superpuestas sin dejar el editor.

### Fase 19: terreno más fino y herramientas de forma

**Hecho.**
- **Detalle.** En *Dibujar suelo → Tamaño del mapa* se elige «Normal» (celda
  de 50) o «Fino» (25). `resampleTerrain` reparte alturas, suelo, agua y
  materiales interpolados sin cambiar lo que mide el mapa. Con hasta 400
  celdas por lado: 20.000 unidades en normal, 10.000 en fino.
- **Acantilados.** «Pintar acantilados» (en *Pintar el suelo*) pinta del
  material elegido todas las pendientes desde la que se indique, con el borde
  fundido.
- **Montañas y hoyos:**
  - «Ruido» (terreno irregular, igual en el lado espejo);
  - sellos de un clic: «Colina», «Cráter» (con borde) y «Meseta» (llana con
    laderas).
- **Herramienta «Trazos».** Puntos con clic, Retroceso quita el último, Intro
  o «Aplicar» aplica. Modos:
  - camino (pinta y allana un poco);
  - río (cauce hundido con agua);
  - calzada (paso elevado con laderas);
  - zona de suelo y hueco (contorno cerrado a mano).

  Con anchura y hondura o altura, y en espejo en los mapas simétricos.
- **Pendiente:** paredes de terreno verticales de verdad. Con celda 25 y
  «Pintar acantilados» se acerca; los salientes y paredes rectas salen con
  mallas propias o recortes (fases 17 y 18).
- Celda de 25 opcional y mapas de hasta unos 8.000 × 8.000 (con la
  simplificación de la fase 15).
- Acantilados verticales de verdad: pared de terreno con textura de roca y
  colisión de pared, en vez de una pendiente muy empinada.
- Herramientas:
  - camino o río por curva con anchura y perfil;
  - pincel de ruido;
  - erosión o suavizado de bordes;
  - sellos de forma (claro, cráter, meseta) y contorno libre del suelo
    dibujado a mano.

### Fase 20: rutas, cámara y ambiente

**Hecho.**
- **Caminos de acarreo calculados sobre la colisión real:**
  - casillas de 100 con los triángulos;
  - en cada punto de la red, todas las alturas que se pisan (hasta ~60°)
    con al menos 45 de sitio encima, así que hay varias capas (mallas
    sobre el terreno);
  - un enlace solo existe si la cuesta es de ~30° o menos y hay suelo
    seguido a la altura de la línea.

  Mallas que se pisan, recortes y terreno entran solos.
- **Caminos a mano** (en *Trazos*):
  - «Unir caminos»: dos puntos que se enlazan siempre, unidos también al
    punto de la red más cercano a su altura (`routes.links`);
  - «Sin caminos»: contorno donde no hay red (`routes.blocks`);
  - se listan, se pueden quitar, se ven en la vista y el espejo los repite
    en el lado de J2.
- **Ambiente** (junto a «Como en»):
  - brillo (luz principal y ambiente), color de la luz y distancia de la
    niebla, sobre la luz del escenario elegido;
  - se aplica a todas las horas del `dayMgr` del `.ini`
    (`environment.brightness`, `tint` y `fog`).
- **Comprobado:**
  - en las 12 plantillas compila en unos 0,1 s;
  - una plataforma de malla a 100 de alto tiene 9 puntos de camino encima;
  - los enlaces y zonas se guardan, se leen y cambian la red.
- **Pendiente:** la cámara en sitios cerrados (túneles, troncos) solo se puede
  comprobar en el juego.
- Rutas de acarreo calculadas sobre la colisión real (pendiente máxima, agua,
  alturas superpuestas), con edición manual de nodos y enlaces, como los
  caminos que llevan los .mod originales (`trailingIni`).
- Luz y niebla por hora del día copiadas de cualquier escenario original, o
  editables (el `dayMgr` de los .ini).
- Comprobar la cámara en zonas cerradas (túneles y troncos).

### Fase 21: prueba de aceptación

**Hecho.**
- **Mapas.** `user/maps/recreado_lugar_del_impacto` y
  `user/maps/recreado_bosque_esperanza`, para solo y coop.
- **Cómo se montaron** (con las funciones del editor, por programa):
  - mapa nuevo sin espejo y terreno sin suelo;
  - geometría: un recorte del escenario entero, puesto como objeto que se
    pisa;
  - objetos y enemigos: los generadores del primer día, en su sitio (57 y 119);
  - misión: capitanes, nave, cebollas (la roja dormida en el Lugar del
    impacto; la amarilla por descubrir en el Bosque) y piezas (1 y 7);
  - luz del escenario original («Como en») y su música;
  - caminos automáticos (fase 20).
- **Resultado:**
  - validan en solo y coop y compilan (6.408 y 24.630 vértices);
  - conservan la colisión original exacta: 2.968 y 8.535 triángulos, con sus
    códigos de agua y terreno;
  - todas las piezas tienen camino hasta la nave;
  - todo lo de la misión y todos los generadores tienen suelo debajo;
  - el render coincide con el original.
- **Arreglos que salieron de la prueba:**
  - los generadores puestos con «Objetos del juego» en un mapa normal no se
    guardaban (solo en el modo escenario);
  - la validación rechazaba los recortes («no es un .mod»);
  - un mapa cuyo suelo es solo una malla daba «sin suelo debajo» (ahora lo
    comprueba el editor con el mapa compilado: altura y suelo salen de él);
  - los recortes usaban como colisión los triángulos dibujados, así que el
    agua era suelo: ahora llevan la colisión original con sus códigos.
- **Lo que no es igual que en el juego:**
  - los caminos, que son automáticos;
  - las reglas de la misión, que son las del editor;
  - solo el primer día de generadores;
  - sin escenas ni diario de la historia.
- Rehacer el Lugar del impacto completo y un día del Bosque de la Esperanza
  solo con el editor: geometría (fase 17), objetos y enemigos (fase 16),
  rutas (fase 20). Comparar memoria y rendimiento en PC y Android.

## Orden recomendado

15 → 16 → 17 (empezando por «partir de un escenario original», que es lo más
rápido para tener niveles iguales que los originales) → 18 → 19 → 20 → 21.

## Riesgos

- Memoria: montones fijos del port, sobre todo en Android, con mallas y
  gestores más grandes.
- Codificar texturas al formato de GameCube (CMPR) sin que se vean mal.
- Rutas automáticas en geometría arbitraria (si fallan, los Pikmin no
  acarrean).
- Cambiar el tamaño de los gestores del motor no debe afectar a la historia
  original ni al VS de serie.

## Mapas prehechos rehechos

Hechos con todo lo anterior.

**Plantillas** (`templates.cpp`):
- detalle de celda 25;
- contorno orgánico en las antiguas y relieve fino con ruido en todas;
- acantilados de roca automáticos;
- piezas de los niveles del juego (`officialPieces`: 23 recortes «solo el
  objeto»), colocadas solas en sitios abiertos y llanos, nunca tapando
  pasillos ni rampas;
- plantas e hierba del juego (`gens`, si hay datos del juego);
- luz y niebla por tema.

**Arena de serie** (`maps/vs/impact_arena`, `impactArena()`):
- misma distribución de juego;
- terreno fino con lomas, charcas como agua del terreno, contorno irregular,
  piezas y decoración;
- sin objetos del juego (va dentro del ejecutable).

**Arreglos y novedades del editor:**
- red de caminos alineada al mundo, simétrica en los mapas espejo; antes un
  lado podía quedar sin conexión en pasillos estrechos;
- recorte «solo el objeto» (`|obj`);
- sección «Piezas de los niveles del juego» en *Poner cosas*.

**Comprobado:**
- todas validan y compilan;
- todo lo que se coge tiene camino, salvo lo que está tras una puerta o un
  bloque a propósito.

