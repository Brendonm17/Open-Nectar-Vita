# Plan: campañas en paquete con mapa de zonas, y añadidos del creador de niveles

Estado: **fases 7 a 14 hechas, y lo aplazado (cámara en disparadores, ratón/táctil en el mapa de zonas, Pikmin por fase, filtro por día en el editor)** (con el editor de campañas y los paquetes .nectarpack; zip propio en lugar de miniz) (paquete, ranuras, mapa de zonas, construcciones persistentes, cebollas enterradas, reglas por enemigo, resultados, música y horas, modificadores, disparadores con condiciones y acciones, oleadas y límite de tiempo); el resto, planificado. Pikmin 1 (`games/pikmin1`), editor
`tools/mapmaker`, formato `libs/nectar_mapcore`.

## Punto de partida (fases 1–6, ya hechas)

- `map.json` formato 2: `modes` (vs/solo/coop), `entities`, sección `story`
  (capitanes, nave, cebollas, piezas, reglas, medallas, disparadores).
- Juego: menú «Custom Maps», misiones solo y coop local, varios días con
  guardado (`user/saves/maps/<id>/`), récords y medallas, disparadores
  (mensaje + aparecer cosas), campañas simples (`user/campaigns/<id>/campaign.json`
  con una lista de misiones), `--mission-map` para probar desde el editor.
- Editor: modos, elementos de misión, reglas, medallas, disparadores, «Probar»
  por modo.
- Módulos clave: `pc_port/mods/pc_mission.*`, `pc_map_catalog.*`,
  `pc_vs_arena.*` (sirve la arena compilada por rutas virtuales),
  `gameCoreSection.cpp` (`pcMissionSetup`, `pcMissionUpdate`,
  `pcCustomSpawn*`), selector en `pc_settings.cpp`.

## Decisiones tomadas

1. **Modelo de campaña: como la historia del juego original.**
   - Contador de **días compartido**; los **Pikmin y las cebollas son de la
     campaña**, no de cada nivel.
   - Cada **zona** es un nivel (`map.json`). Cada visita es un día; al
     anochecer se vuelve al **mapa de zonas** y la nave vuela a otra zona
     desbloqueada.
   - Lo recogido, derrotado o construido en una zona **se queda así** entre
     visitas.
   - Cada zona puede tener un **objetivo opcional** («derrota al jefe»).
     Cumplirlo marca la zona como completada, y eso puede usarse en las reglas
     de desbloqueo.
   - **Final** definido por el creador (todas las piezas / una zona concreta /
     X días…), con límite de días opcional y final malo si se agota.
   - Las misiones sueltas de un día siguen valiendo: una zona con objetivo y
     un solo día.
2. **Las campañas van en paquete** (carpeta o archivo). Desde el menú se
   elige la campaña, no niveles sueltos.
3. **3 ranuras de guardado por campaña.**
4. **Pendiente de decidir:** qué hacer con los mapas sueltos de `user/maps`.
   - Propuesta: que el menú los muestre automáticamente como paquetes de una
     sola zona, sin mapa de zonas.
   - Alternativa: quitarlos del menú y que solo se jueguen desde el editor.

---

## A. Paquete de campaña (formato)

```
user/campaigns/<id>/            ← o <id>.nectarpack (zip con lo mismo)
  campaign.json                 ← reglas, zonas, puntos, desbloqueos, final
  worldmap.png                  ← opcional; si no, el fondo del juego
  thumb.png
  zones/<zona>/map.json         ← cada nivel (+ thumb.png)
```

- Se escribe `nectar::CampaignDoc` en `nectar_mapcore` (lectura, escritura y
  validación). Sustituye al `nectar::Campaign` simple de ahora.
- `campaign.json`:
  - `meta`: nombre, autor, versión y textos en es/en (`name_es`/`name_en`…).
  - `worldmap`: `{"image":"game"}` (fondo del juego, `w_map.bti`) o
    `{"image":"worldmap.png"}`.
  - `zones[]`:
    - id, nombre y carpeta;
    - punto en la imagen, en % (0–1, independiente de la resolución);
    - foto: `game:<snap>` o un PNG;
    - música y objetivo opcional;
    - `unlock` (sección C);
    - `startUnlocked`.
  - `paths[]`: pares de zonas unidas (la línea de puntos y la navegación con
    mando).
  - `start`: zona inicial, cebollas iniciales, Pikmin iniciales por color y
    fase, día inicial.
  - `ending`: condición de victoria, `dayLimit` opcional y textos de final
    bueno y malo.
- Validación:
  - Cada pieza de la nave aparece **una sola vez en toda la campaña** (el
    juego tiene 30).
  - Toda zona se puede desbloquear: grafo de reglas sin ciclos imposibles,
    piezas y cebollas que existen en alguna zona.
  - Ids sin `/`, `\`, `..`.
  - Cada zona tiene que valer para solo o coop (o para ambos, según el
    paquete).
- Menú «Custom Maps»: lista **paquetes**, con miniatura, autor, número de
  zonas y progreso de cada ranura.

## B. Pantalla del mapa de zonas (juego)

**Lo que hay en el juego original**
- `zen::DrawWorldMap` (`src/plugPikiYamashita/drawWorldMap.cpp`, ~3000
  líneas): 5 puntos con enlaces escritos a mano (`WorldMapCoursePointMgr`) y
  BLOs fijos (`w_point.blo`, `w_line.blo`…). No se puede adaptar a N puntos
  libres.
- Texturas reutilizables (`assets/dataDir/screen/eng_tex/`):

  | Textura | Uso |
  |---|---|
  | `w_map.bti` | fondo del mapa (lo usa `w_back.blo`) |
  | `i_rocket2.bti` | icono de la nave (`w_icon.blo`) |
  | `r_onion2.bti`, `b_onion2.bti`, `y_onion2.bti` | iconos de cebollas |
  | `pr_snap.bti`, `fo_snap.bti`, `ca_snap.bti`, `ya_snap.bti`, `ga_snap.bti` | fotos de las zonas (`w_moni.blo`) |
  | `line2.bti` | línea de puntos de los caminos (`w_line.blo`) |
  | `map0_i4.bti` … `map4_i4.bti` | «tierras» de cada zona |

**Cómo hacerlo**
- Pantalla propia, dibujada con lo que ya usa el port:
  `pc_settings_p2d_image`, `pc_art_texture("@ruta.png")` para las PNG del
  creador, y `gsys->loadTexture(...)` para las BTI del juego.
- Contenido:
  - Fondo y caminos con puntos.
  - Zonas en tres estados: bloqueada (gris o candado), disponible y
    completada (estrella).
  - La nave sobre la zona actual (flotando); al elegir otra, viaja en curva
    hasta ella.
  - Animación de desbloqueo: el punto aparece y el camino se dibuja poco a
    poco, como `CourseAppearState` del original.
  - Panel lateral:
    - foto, nombre y objetivo de la zona;
    - piezas de la zona encontradas (x/y), como `WorldMapPartsInfoMgr`;
    - día y Pikmin por color;
    - requisitos de lo que sigue bloqueado.
- Control: con mando se navega por los caminos (el vecino más cercano en la
  dirección pulsada). También ratón y táctil.
- Flujo: en el original el fin del día lleva a `ONEPLAYER_MapSelect`. Se
  engancha `MapSelectSetupSection` (`src/plugPikiColin/mapSelect.cpp`):
  - con una campaña activa, sale esta pantalla en lugar de `DrawWorldMap`;
  - al elegir zona: `pc_mission_select` de la zona + `pc_vs_arena_prepare`,
    y luego `ONEPLAYER_NewPikiGame`;
  - el fin de un día de campaña (`pc_mission_take_exit`) va a
    `ONEPLAYER_MapSelect`, no al título.

## C. Reglas de desbloqueo

Una zona se abre cuando se cumple **todo** su bloque `unlock`; un `any`
permite alternativas.

| Regla | Ejemplo |
|---|---|
| `parts` | 5 piezas en total |
| `part` | la pieza `uf05` |
| `pikmin` | 30 amarillos (o en total) |
| `onion` | tener la cebolla azul |
| `zone` | zona X completada (o visitada) |
| `day` | desde el día 4 |
| `medal` | oro en la zona X |

- Se evalúan al volver al mapa de zonas.
- Lo recién abierto se anima y se anuncia («Desbloqueado: …»).
- El editor enseña qué regla falta para abrir cada zona.

## D. Estado de la campaña y 3 ranuras

- `user/saves/campaigns/<id>/slot1..3.txt`.
- Pantalla de ranuras al elegir un paquete: día, piezas, Pikmin, zonas
  abiertas y completadas, y fecha. Acciones: nueva / continuar / copiar /
  borrar. Mismo estilo que el selector actual.
- Contenido de una ranura:
  - Día, zona actual y número de jugadores (1 o 2, elegido al crearla).
  - Cebollas que se tienen y Pikmin por color y fase (hoja, capullo, flor).
  - Piezas recogidas (y en qué zona).
  - Zonas abiertas, completadas y su medalla.
  - Estado de cada zona: lo que hoy guarda `Run` (piezas, pastillas, jefes,
    objetos, disparadores) más el estado de las construcciones (punto 6) y
    los enemigos muertos (punto 7).
  - Disparadores de la campaña ya saltados.
  - Modificadores elegidos (punto 13).
- Versión: se guarda la versión de cada zona (la huella de su `map.json`).
  Si el creador cambia una zona, **solo esa zona** se reinicia; el resto de
  la partida se conserva. Hoy se pierde toda la partida.

---

## 1. Editor de campañas

- Vista nueva en el editor, «Campaña». Al abrir o crear un paquete aparece la
  imagen del mapa a pantalla completa.
  - La imagen se elige entre una PNG propia o «la del juego» (`w_map.bti`,
    decodificada con `libs/nectar_assets/src/image.cpp`).
  - Los puntos se colocan arrastrando y los caminos se trazan uniendo dos
    puntos.
- Panel de zona:
  - nombre;
  - «Editar nivel», que abre su `map.json` en el editor actual;
  - reglas de desbloqueo: lista de condiciones con desplegables;
  - foto: una `*_snap.bti` del juego o la miniatura del nivel;
  - música.
- Panel de campaña: estado inicial, final, límite de días, y textos de
  introducción y final.
- Comprobador del paquete (sección A), con «arreglar» cuando se pueda, como
  la lista de problemas actual.
- Probar: `--campaign <carpeta> --zone <id> [--state <archivo>]` arranca en
  esa zona con un estado de prueba. Reutiliza el arranque directo de
  `--mission-map` (`pc_mission_direct_*`).

## 2. Estado entre zonas

Lo cubre D. En el juego, al entrar en una zona:
- Las cebollas que se tienen bajan junto a la nave, en los puntos
  `story.onions` no enterrados del nivel, con su recuento de Pikmin (lo que
  hace hoy `pc_mission_onion_pikmin`, pero con datos de la campaña).
- Las cebollas enterradas del nivel son las que se pueden descubrir allí.
- En campaña, `story.rules.startPikmin` del nivel no se usa: los Pikmin
  salen del estado.

## 3. Más acciones en los disparadores

`DocTrigger` pasa a tener una lista de **acciones**. Además de las actuales
(mensaje y hacer aparecer cosas):

- `break <índice>`: romper una construcción.
  - Compuerta: `BuildingItem` a completado.
  - Puerta: `DoorItem` a `Vanishing`.
  - Roca: `HinderRock` con `mState = 2` y movida al destino, como en `doLoad`.
  - Puente: `Bridge::setStageFinished(i, true)` + `flatten()`.
- `win` / `lose`: llaman a `finish(...)` de `pc_mission`.
- `timer`: cuenta atrás en el HUD; al llegar a cero, ejecuta otras acciones.
- `spawn_at <elemento>`: hacer aparecer algo junto a un elemento del mapa, no
  en unas coordenadas fijas.
- `camera`: enfocar un punto unos segundos (`AttentionCamera` / `cameraMgr`
  en `GameCoreSection`). Es lo más arriesgado; va al final.
- `textbox`: cuadro de texto con el retrato del capitán. Superposición
  propia, porque `ogMessage`/`ogTutorial` leen textos fijos por id. Pausa el
  juego y se cierra con A.

## 4. Condiciones combinadas y repeticiones

- `conditions: [...]` con `"match": "all" | "any"`.
- Condiciones nuevas:
  - `area`: un capitán dentro de un círculo;
  - `object`: una construcción terminada o rota;
  - `timer`: N segundos desde el inicio o desde otro disparador;
  - `pikmin`: N de un color.
- `repeat` + `cooldown`: oleadas, néctar periódico…
- Se evalúan en `pc_mission_update`. `gameCoreSection` le pasa los datos
  (posición de los capitanes, estado de `sCustomObjects`).
- Editor: lista de condiciones en el panel de disparadores. Las de área se
  dibujan en la vista 3D y se mueven con el gizmo.

## 5. Cebollas enterradas de verdad

- Lo que hay en el juego:
  - sin `setBootContainer`, una cebolla arranca dormida en `GOAL_BootInit`;
    `GoalItem::startBoot()` la despierta (`src/plugPikiKando/goalItem.cpp`,
    ~línea 700);
  - el descubrimiento lo lanza `Navi::demoCheck()`
    (`src/plugPikiKando/demoInvoker.cpp`), pero solo en zonas fijas: roja en
    Impact Site, azul en Forest Navel, amarilla en Forest of Hope.
- Plan:
  - En misiones y campañas, a las enterradas no se les llama a
    `setBootContainer`.
  - En `demoCheck`: si hay misión y un capitán está a menos de
    `demoParms->mParms.mOnionBootTriggerRadius()` de una cebolla dormida,
    `startBoot()` + `GOAL_BootEmit` (la primera semilla, como en el salto de
    cinemática del port, `demoEvent.cpp`).
  - Opcional: lanzar la cinemática original `DEMOFLAG_Discover*Onyon`. Antes
    hay que comprobar que su cámara se coloca respecto a la cebolla.
  - En campaña, descubrirla suma la cebolla al estado (y la regla `onion`
    puede abrir zonas).

## 6. Construcciones que se mantienen

- Lo que hay en el juego: cada objeto ya sabe guardarse y recuperarse.
  - `Bridge::doSave/doLoad` (`workObject.cpp`): avance de cada tramo; al
    cargar, vuelve a montar los tramos terminados.
  - `HinderRock::doSave/doLoad`: si ya se empujó, la pone en su destino.
  - `BuildingItem::doSave/doLoad`: el estado de las compuertas.
- Plan:
  - Al anochecer, cada objeto de `sCustomObjects` (ampliado a puentes y
    rocas) escribe su estado con `doSave` en un `RamStream`, que se guarda en
    base64 por índice.
  - Al montar el nivel, tras `startAI`, se aplica `doLoad` con ese estado.
  - Así se guarda también un puente a medias, no solo uno terminado.

## 7. Reglas por enemigo

- En `DocEnemy` / `MapEnemy`:
  - `respawn`: `always` (como ahora), `never` o `days:N`;
  - `fromDay` / `untilDay`.
- Juego: `Run.bosses` pasa a guardar a todos los enemigos muertos y en qué
  día. `pcCustomSpawnEntities` decide con estas reglas; en campaña el día es
  el global.
- Editor: desplegable en el panel del enemigo y un filtro de vista «qué hay
  el día N».

## 8. Límite de tiempo y oleadas

- `story.rules.timeLimit` (segundos): cuenta atrás en el HUD; al llegar a
  cero, derrota o fin del día (lo elige el creador).
- `dayLength`: duración del día, cambiando la velocidad del reloj. Falta
  localizar el multiplicador en `WorldClock` / `parameters`.
- Oleadas: se montan con los puntos 3 y 4 (disparador `timer` + `repeat` +
  aparecer enemigos), más un objetivo nuevo «aguantar N oleadas». No necesita
  un sistema aparte.

## 9. Pantalla de resultados

- Superposición propia, como la pantalla final del VS (`vsEndScreenShown` en
  `pc_settings.cpp`), en lugar del aviso de 5 s.
- Datos:
  - resultado, medalla y récord;
  - tiempo y días;
  - piezas y pastillas;
  - Pikmin nacidos y perdidos (`GameStat::bornPikis`, `deadPikis`,
    `victimPikis`);
  - enemigos derrotados (`GameStat::killTekis`).
- Botones:
  - Reintentar: `ONEPLAYER_NewPikiGame` con el estado del inicio del día;
  - Continuar: mapa de zonas o día siguiente;
  - Título.
- Resumen de fin de día: Pikmin por color, salvados y perdidos, como el
  original. Más adelante se podría reutilizar `ogResult.cpp`.

## 10. Importar y exportar, y miniaturas

- No hay librería zip en el repositorio. Se añade **miniz** (un archivo,
  licencia MIT) en `libs/third_party`, para el juego y el editor.
- Editor: «Exportar paquete» genera un `.nectarpack` con la carpeta entera.
- Juego: «Importar» en Custom Maps.
  - Selector de archivos: `pc_file_dialog` en PC, el del sistema en Android.
  - Descomprime en `user/campaigns/<id>/`, rechazando rutas con `..` o
    absolutas (zip-slip).
  - Valida antes de enseñarlo en la lista.
- Miniaturas:
  - Al guardar, el editor hace una vista cenital en su `RenderTarget`, la
    lee con `glReadPixels` y la guarda como `thumb.png` con `stb_image_write`
    (ya está en `libs/nectar_assets/third_party`).
  - En el mapa de zonas, la foto de una zona sale de ahí si el creador no
    elige otra.

## 11. Música y hora de inicio

- Música: `newPikiGame.cpp` (~línea 2245) llama a
  `Jac_SceneSetup(SCENE_Course, stageId)`. En misiones y campañas se pasa la
  zona elegida en `environment.music` (practice, forest, cave, yakushima,
  last).
- Hora de inicio: `cardSelect.cpp` y el paso de día en `newPikiGame.cpp`
  hacen `setTime(mStartHour)`. Se cambia por `environment.startHour`; con
  `endHour` se acortan los días.
- Editor: dos controles más en «Ambiente» (con botón para escuchar la
  música).

## 13. Modificadores

- Se eligen al crear una ranura o al empezar una misión suelta:
  - dificultad: vida de los enemigos, que ya pasa por
    `pc_settings_get_teki_health_pct`;
  - días extra;
  - «sin bajas»: se pierde si muere un Pikmin (`GameStat::deadPikis`);
  - límite de Pikmin en el campo;
  - velocidad de carga, que ya existe en los ajustes.
- Se aplican sobrescribiendo los `pc_settings_get_*` mientras haya misión,
  como ya se hace con `infiniteDay`.
- Los récords se guardan aparte con y sin modificadores, y la ficha dice con
  qué se hizo.

---

## E. Añadidos propuestos además

1. **Diario de campaña**: un texto por zona y día que se muestra al
   anochecer, como el diario de Olimar (`ogDiary`). Mucha historia por poco
   trabajo.
2. **Introducción y final** con texto e imagen del paquete; final malo si se
   acaban los días.
3. **Radar de piezas por zona** en el mapa (x/y encontradas), como el
   original.
4. **La nave cambia por fuera** según las piezas recogidas: subir
   `playerState->mShipUpgradeLevel` (cambian los propulsores).
5. **Textos en dos idiomas** dentro del paquete (es/en).
6. **Probar con un estado elegido** desde el editor: «día 5, 40 rojos,
   cebolla azul».
7. **Comprobador de campaña**: avisa si una zona no se puede abrir o si una
   pieza se repite entre zonas.
8. **Puntos de interés en el mapa** que solo enseñan texto o imagen (pistas,
   lore).

## F. Orden de las fases

| Fase | Contenido | Depende de |
|---|---|---|
| 7 | Formato del paquete + estado de campaña + 3 ranuras (A, D, 2) | — |
| 8 | Mapa de zonas + flujo `MapSelect` + reglas de desbloqueo (B, C) | 7 |
| 9 | Construcciones (6), cebollas enterradas (5), reglas de enemigos (7) | 7 |
| 10 | Pantalla de resultados (9) + música y hora (11) + modificadores (13) | — |
| 11 | Disparadores: acciones (3) + condiciones (4) + tiempo y oleadas (8) | — |
| 12 | Editor de campañas (1) + comprobador | 7, 8 |
| 13 | miniz: importar y exportar + miniaturas (10) | 7, 12 |
| 14 | Añadidos de E | según el caso |

**Riesgos**
- Alto:
  - el enganche en `MapSelect`, porque cambia el flujo de fin de día;
  - la cámara de los disparadores;
  - la cinemática original de descubrir cebollas.
- Bajo: puntos 5 y 6, que usan funciones que el juego ya tiene y usa en la
  historia.

## Fuera de este plan

- Coop online en misiones y campañas (netplay).
- Repositorio online de mapas y tablas de récords.
- Llevar el creador a Pikmin 2.
