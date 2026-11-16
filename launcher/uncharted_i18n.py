"""Small launcher translation table with English fallback."""
import locale

from bbport_lang import table as shared_translations
from uncharted_i18n_extra import EXTRA_TRANSLATIONS


LANGUAGES = [
    ('', 'Automatic (Windows)'),
    ('en', 'English'),
    ('es', 'Español'),
    ('pt', 'Português'),
    ('fr', 'Français'),
    ('de', 'Deutsch'),
    ('it', 'Italiano'),
    ('ja', '日本語'),
]


def language_code(value=''):
    if value in {code for code, _name in LANGUAGES if code}:
        return value
    current = locale.getlocale()[0] or 'en'
    code = current.split('_', 1)[0].lower()
    return code if code in TRANSLATIONS else 'en'


TRANSLATIONS = {
    'es': {
        'Automatic (Windows)': 'Automático (Windows)', 'Play': 'Jugar', 'Stop': 'Detener',
        'Game': 'Juego', 'Display': 'Pantalla', 'Controls': 'Controles', 'Advanced': 'Avanzado',
        'Log': 'Registro', 'Library': 'Biblioteca', 'Double-click a cover to launch':
        'Haz doble clic en una portada para iniciar', 'Game folder': 'Carpeta del juego',
        'The folder with eboot.bin': 'La carpeta que contiene eboot.bin',
        'Saves folder': 'Carpeta de partidas', 'Console': 'Consola', 'Language': 'Idioma',
        'Launcher language': 'Idioma del launcher', 'Interface language': 'Idioma de la interfaz',
        'This changes only the launcher. The game language is selected on the Game page.':
        'Esto cambia solo el launcher. El idioma del juego se elige en la página Juego.',
        'Player name': 'Nombre del jugador',
        'Fullscreen': 'Pantalla completa', 'Presentation': 'Presentación',
        'Frame rate': 'Frecuencia de cuadros', 'Resolution': 'Resolución',
        'Window size': 'Tamaño de ventana', 'Upscaling': 'Reescalado',
        'Sharpening': 'Nitidez', 'Anti-aliasing': 'Suavizado de bordes',
        'Anti-aliasing sharpness': 'Nitidez del suavizado',
        'Frame generation': 'Generación de cuadros', 'Multiplier': 'Multiplicador',
        'Show the FPS counter': 'Mostrar contador de FPS', 'Mouse': 'Ratón',
        'Look around with the mouse': 'Mover la cámara con el ratón',
        'Mouse sensitivity': 'Sensibilidad del ratón',
        'Invert the mouse vertically': 'Invertir verticalmente el ratón',
        'Keyboard and mouse buttons': 'Teclado y botones del ratón',
        'Controller': 'Control', 'Default keys': 'Teclas predeterminadas',
        'Default controller buttons': 'Botones predeterminados del control',
        'Maintenance and options for testing.': 'Mantenimiento y opciones para pruebas.',
        'Close the launcher when the game starts': 'Cerrar el launcher al iniciar el juego',
        'Environment': 'Entorno', 'Shader cache': 'Caché de shaders', 'Updates': 'Actualizaciones',
        'Clear shader cache': 'Borrar caché de shaders',
        'Open the saves folder': 'Abrir carpeta de partidas',
        'Look for a new version when the launcher starts': 'Buscar actualizaciones al abrir el launcher',
        'Check for updates': 'Buscar actualizaciones', 'Releases page': 'Página de versiones',
        'Browse…': 'Examinar…', 'Open': 'Abrir', 'Add to Steam': 'Añadir a Steam',
        'Desktop shortcut': 'Acceso directo en el escritorio', 'Copy': 'Copiar',
        'Choose the game folder (the one with eboot.bin) on the Game page.':
        'Elige la carpeta del juego (la que contiene eboot.bin) en la página Juego.',
        'No game folder yet.': 'Aún no se ha elegido la carpeta del juego.',
        'The game is running.': 'El juego está en ejecución.',
        'Native port: the original PS4 game code runs directly on Windows.':
        'Port nativo: el código original de PS4 se ejecuta directamente en Windows.',
        'version': 'versión', 'version {}': 'versión {}',
        '\n— the game exited (code {}) —\n': '\n— el juego terminó (código {}) —\n',
        'AMD RCAS after FSR 1 (only while the image is enlarged).':
        'AMD RCAS después de FSR 1 (solo cuando se amplía la imagen).',
        'Choose the game folder with eboot.bin.': 'Elige la carpeta del juego que contiene eboot.bin.',
        'Click a box, then press a key or a mouse button (the wheel counts too). ✕ removes a binding. Each action can have two.':
        'Haz clic en una casilla y pulsa una tecla o botón del ratón (la rueda también cuenta). ✕ elimina una asignación. Cada acción puede tener dos.',
        'Close the game before updating.': 'Cierra el juego antes de actualizar.',
        'Compiled shaders are kept so the game does not stutter again. Clear the cache only when the picture is broken after an update.':
        'Los shaders compilados se conservan para evitar nuevos tirones. Borra la caché solo si la imagen falla después de una actualización.',
        'Could not add the game to Steam: {}': 'No se pudo añadir el juego a Steam: {}',
        'Could not check for updates: {}': 'No se pudo buscar actualizaciones: {}',
        'Could not install the update. Download it from the releases page.':
        'No se pudo instalar la actualización. Descárgala desde la página de versiones.',
        'Could not start: {}': 'No se pudo iniciar: {}',
        'Downloading version {}… {}%': 'Descargando la versión {}… {}%',
        'Empty: the user folder next to the launcher.': 'Vacío: se usará la carpeta user junto al launcher.',
        'Extra variables for the game, NAME=value separated by spaces.':
        'Variables adicionales para el juego, NOMBRE=valor separadas por espacios.',
        'FSR 1 keeps edges sharp when the image is enlarged; bilinear is softer.':
        'FSR 1 mantiene los bordes nítidos al ampliar la imagen; bilineal es más suave.',
        'FSR 3 makes one frame (2×). DLSS makes up to three on a GeForce RTX 50 (4×), one on an RTX 40.':
        'FSR 3 genera un cuadro (2×). DLSS genera hasta tres en una GeForce RTX 50 (4×) y uno en una RTX 40.',
        "Frames made between two of the game's, for smoother motion: 60 FPS shows as 120. Use Mailbox or Immediate presentation and a display of at least that refresh rate. Adds a little input latency; menus and cutscene videos are not generated. Use it with the 60 or 120 FPS frame rate: above about 165 FPS it pauses.":
        'Cuadros creados entre dos del juego para un movimiento más fluido: 60 FPS se muestran como 120. Usa presentación Mailbox o Inmediata y una pantalla con al menos esa frecuencia. Añade un poco de latencia; no se generan cuadros en menús ni cinemáticas. Úsalo con 60 o 120 FPS: por encima de unos 165 FPS se pausa.',
        "Gameplay keeps its normal speed at every setting; the cutscene videos always play at 60 FPS. Above your display's refresh rate use Mailbox or Immediate.":
        'El juego mantiene su velocidad normal con cualquier ajuste; las cinemáticas siempre se reproducen a 60 FPS. Por encima de la frecuencia de tu pantalla usa Mailbox o Inmediata.',
        'How far the camera turns for a mouse movement; 1.00 is the default.':
        'Cuánto gira la cámara con el movimiento del ratón; 1.00 es el valor predeterminado.',
        'How the game window is shown.': 'Cómo se muestra la ventana del juego.',
        'Install version {} now? The launcher closes, installs it and opens again. Saves and settings are kept.':
        '¿Instalar ahora la versión {}? El launcher se cerrará, la instalará y volverá a abrirse. Se conservarán las partidas y los ajustes.',
        'Invert the right stick vertically': 'Invertir verticalmente el stick derecho',
        'Keyboard, mouse and controller. They work together; changes apply when the game starts.':
        'Teclado, ratón y control. Funcionan a la vez; los cambios se aplican al iniciar el juego.',
        'Mailbox shows each frame as soon as it is ready without tearing; FIFO waits for the display.':
        'Mailbox muestra cada cuadro en cuanto está listo sin tearing; FIFO espera a la pantalla.',
        'Not in this folder: ': 'No está en esta carpeta: ',
        'Preparing the game; it opens in its own window…': 'Preparando el juego; se abrirá en su propia ventana…',
        'Press a key…': 'Pulsa una tecla…', 'Ready: {}': 'Listo: {}',
        'Set your game folder on the Game page': 'Elige la carpeta del juego en la página Juego',
        'Shader cache cleared. The next start stutters for a while as it is rebuilt.':
        'Caché de shaders borrada. El próximo inicio tendrá tirones mientras vuelve a generarse.',
        'Shortcut created on the desktop.': 'Acceso directo creado en el escritorio.',
        'Shown where the game uses the PSN name.': 'Se muestra donde el juego utiliza el nombre de PSN.',
        'Starting the next game of the collection…': 'Iniciando el siguiente juego de la colección…',
        'Steam did not close. Close it yourself and try again.': 'Steam no se cerró. Ciérralo e inténtalo de nuevo.',
        'Steam has to close while the game is added; it starts again afterwards. Close Steam now?':
        'Steam debe cerrarse mientras se añade el juego; después volverá a abrirse. ¿Cerrar Steam ahora?',
        'Steam was not found on this PC (or nobody has signed in to it yet).':
        'No se encontró Steam en este PC (o nadie ha iniciado sesión todavía).',
        "Temporal anti-aliasing at the games' 1920 × 1080, with their own motion vectors: smoother edges and less shimmering than the games' own. DLSS needs a GeForce RTX; FSR 4 runs best on a Radeon RX 7000/9000 or a GeForce RTX 40/50.":
        'Suavizado temporal a 1920 × 1080 con los vectores de movimiento del juego: bordes más suaves y menos parpadeo que el suavizado original. DLSS requiere una GeForce RTX; FSR 4 funciona mejor en Radeon RX 7000/9000 o GeForce RTX 40/50.',
        'The collection is in your Steam library. It starts with the settings saved in this launcher.':
        'La colección está en tu biblioteca de Steam. Se iniciará con los ajustes guardados en este launcher.',
        'The cursor stays in the game window; hold Left Alt (or switch windows) to free it. Insert opens the in-game settings menu.':
        'El cursor permanece en la ventana del juego; mantén Alt izquierdo (o cambia de ventana) para liberarlo. Insert abre los ajustes dentro del juego.',
        'The folder with eboot.bin, big2-ps4_Shipping.elf and big3-ps4_Shipping.elf.':
        'La carpeta con eboot.bin, big2-ps4_Shipping.elf y big3-ps4_Shipping.elf.',
        'The game stopped: {}': 'El juego se detuvo: {}',
        'The games render at 1920 × 1080. A larger window, or fullscreen on a 1440p or 4K screen, gets the image upscaled to fit.':
        'Los juegos renderizan a 1920 × 1080. En una ventana mayor o a pantalla completa en 1440p o 4K, la imagen se reescala para ajustarse.',
        'The language the PS4 would be set to; the games use it for text and speech.':
        'El idioma que tendría configurado la PS4; los juegos lo usan para el texto y las voces.',
        'This game is not in the chosen folder.': 'Este juego no está en la carpeta elegida.',
        'Update': 'Actualizar', 'Update failed: {}': 'La actualización falló: {}',
        'Version {} is available.': 'La versión {} está disponible.', "What's new": 'Novedades',
        'Where saves and the shader cache are kept': 'Dónde se guardan las partidas y la caché de shaders',
        'Which controller button gives each PS4 button (Xbox / PlayStation names).':
        'Qué botón del control corresponde a cada botón de PS4 (nombres de Xbox / PlayStation).',
        'You have the latest version ({}).': 'Tienes la versión más reciente ({}).',
        'Your display runs at {} Hz.': 'Tu pantalla funciona a {} Hz.',
        'Your own decrypted dump of the collection (CUSA02343, CUSA02320, …).':
        'Tu propia copia descifrada de la colección (CUSA02343, CUSA02320, …).',
        'by {}': 'por {}', 'see the Log page': 'consulta la página Registro',
    },
    'pt': {
        'Automatic (Windows)': 'Automático (Windows)', 'Play': 'Jogar', 'Stop': 'Parar',
        'Game': 'Jogo', 'Display': 'Tela', 'Controls': 'Controles', 'Advanced': 'Avançado',
        'Log': 'Registro', 'Library': 'Biblioteca', 'Double-click a cover to launch':
        'Clique duas vezes em uma capa para iniciar', 'Game folder': 'Pasta do jogo',
        'The folder with eboot.bin': 'A pasta que contém eboot.bin',
        'Saves folder': 'Pasta de jogos salvos', 'Console': 'Console', 'Language': 'Idioma',
        'Launcher language': 'Idioma do launcher', 'Interface language': 'Idioma da interface',
        'This changes only the launcher. The game language is selected on the Game page.':
        'Isso altera apenas o launcher. O idioma do jogo é escolhido na página Jogo.',
        'Player name': 'Nome do jogador', 'Fullscreen': 'Tela cheia', 'Presentation': 'Apresentação',
        'Frame rate': 'Taxa de quadros', 'Resolution': 'Resolução',
        'Window size': 'Tamanho da janela', 'Upscaling': 'Redimensionamento',
        'Sharpening': 'Nitidez', 'Anti-aliasing': 'Antisserrilhamento',
        'Anti-aliasing sharpness': 'Nitidez do antisserrilhamento',
        'Frame generation': 'Geração de quadros', 'Multiplier': 'Multiplicador',
        'Show the FPS counter': 'Mostrar contador de FPS', 'Mouse': 'Mouse',
        'Look around with the mouse': 'Mover a câmera com o mouse',
        'Mouse sensitivity': 'Sensibilidade do mouse',
        'Invert the mouse vertically': 'Inverter o mouse verticalmente',
        'Keyboard and mouse buttons': 'Teclado e botões do mouse',
        'Controller': 'Controle', 'Default keys': 'Teclas padrão',
        'Default controller buttons': 'Botões padrão do controle',
        'Maintenance and options for testing.': 'Manutenção e opções para testes.',
        'Close the launcher when the game starts': 'Fechar o launcher ao iniciar o jogo',
        'Environment': 'Ambiente', 'Shader cache': 'Cache de shaders', 'Updates': 'Atualizações',
        'Clear shader cache': 'Limpar cache de shaders',
        'Open the saves folder': 'Abrir pasta de jogos salvos',
        'Look for a new version when the launcher starts': 'Procurar atualizações ao abrir o launcher',
        'Check for updates': 'Procurar atualizações', 'Releases page': 'Página de versões',
        'Browse…': 'Procurar…', 'Open': 'Abrir', 'Add to Steam': 'Adicionar à Steam',
        'Desktop shortcut': 'Atalho na área de trabalho', 'Copy': 'Copiar',
        'Choose the game folder (the one with eboot.bin) on the Game page.':
        'Escolha a pasta do jogo (a que contém eboot.bin) na página Jogo.',
        'No game folder yet.': 'Nenhuma pasta de jogo foi escolhida.',
        'The game is running.': 'O jogo está em execução.',
        'Native port: the original PS4 game code runs directly on Windows.':
        'Port nativo: o código original do jogo de PS4 é executado diretamente no Windows.',
        'version': 'versão', 'version {}': 'versão {}',
        '\n— the game exited (code {}) —\n': '\n— o jogo terminou (código {}) —\n',
        'AMD RCAS after FSR 1 (only while the image is enlarged).':
        'AMD RCAS após o FSR 1 (somente quando a imagem é ampliada).',
        'Choose the game folder with eboot.bin.': 'Escolha a pasta do jogo que contém eboot.bin.',
        'Click a box, then press a key or a mouse button (the wheel counts too). ✕ removes a binding. Each action can have two.':
        'Clique em uma caixa e pressione uma tecla ou botão do mouse (a roda também conta). ✕ remove uma atribuição. Cada ação pode ter duas.',
        'Close the game before updating.': 'Feche o jogo antes de atualizar.',
        'Compiled shaders are kept so the game does not stutter again. Clear the cache only when the picture is broken after an update.':
        'Os shaders compilados são mantidos para evitar novas travadas. Limpe o cache somente se a imagem apresentar problemas após uma atualização.',
        'Could not add the game to Steam: {}': 'Não foi possível adicionar o jogo à Steam: {}',
        'Could not check for updates: {}': 'Não foi possível procurar atualizações: {}',
        'Could not install the update. Download it from the releases page.':
        'Não foi possível instalar a atualização. Baixe-a na página de versões.',
        'Could not start: {}': 'Não foi possível iniciar: {}',
        'Downloading version {}… {}%': 'Baixando a versão {}… {}%',
        'Empty: the user folder next to the launcher.': 'Vazio: será usada a pasta user ao lado do launcher.',
        'Extra variables for the game, NAME=value separated by spaces.':
        'Variáveis adicionais para o jogo, NOME=valor separadas por espaços.',
        'FSR 1 keeps edges sharp when the image is enlarged; bilinear is softer.':
        'O FSR 1 mantém as bordas nítidas ao ampliar a imagem; bilinear é mais suave.',
        'FSR 3 makes one frame (2×). DLSS makes up to three on a GeForce RTX 50 (4×), one on an RTX 40.':
        'O FSR 3 gera um quadro (2×). O DLSS gera até três em uma GeForce RTX 50 (4×) e um em uma RTX 40.',
        "Frames made between two of the game's, for smoother motion: 60 FPS shows as 120. Use Mailbox or Immediate presentation and a display of at least that refresh rate. Adds a little input latency; menus and cutscene videos are not generated. Use it with the 60 or 120 FPS frame rate: above about 165 FPS it pauses.":
        'Quadros criados entre dois do jogo para um movimento mais suave: 60 FPS são exibidos como 120. Use apresentação Mailbox ou Imediata e uma tela com pelo menos essa taxa. Adiciona um pouco de latência; menus e vídeos não são gerados. Use com 60 ou 120 FPS: acima de cerca de 165 FPS ele pausa.',
        "Gameplay keeps its normal speed at every setting; the cutscene videos always play at 60 FPS. Above your display's refresh rate use Mailbox or Immediate.":
        'O jogo mantém a velocidade normal em qualquer configuração; os vídeos sempre rodam a 60 FPS. Acima da taxa da tela, use Mailbox ou Imediata.',
        'How far the camera turns for a mouse movement; 1.00 is the default.':
        'Quanto a câmera gira com o movimento do mouse; 1.00 é o padrão.',
        'How the game window is shown.': 'Como a janela do jogo é exibida.',
        'Install version {} now? The launcher closes, installs it and opens again. Saves and settings are kept.':
        'Instalar a versão {} agora? O launcher será fechado, instalará a versão e abrirá novamente. Jogos salvos e configurações serão mantidos.',
        'Invert the right stick vertically': 'Inverter o analógico direito verticalmente',
        'Keyboard, mouse and controller. They work together; changes apply when the game starts.':
        'Teclado, mouse e controle. Funcionam juntos; as alterações são aplicadas ao iniciar o jogo.',
        'Mailbox shows each frame as soon as it is ready without tearing; FIFO waits for the display.':
        'Mailbox mostra cada quadro assim que fica pronto, sem tearing; FIFO espera pela tela.',
        'Not in this folder: ': 'Não está nesta pasta: ',
        'Preparing the game; it opens in its own window…': 'Preparando o jogo; ele abrirá em sua própria janela…',
        'Press a key…': 'Pressione uma tecla…', 'Ready: {}': 'Pronto: {}',
        'Set your game folder on the Game page': 'Escolha a pasta do jogo na página Jogo',
        'Shader cache cleared. The next start stutters for a while as it is rebuilt.':
        'Cache de shaders limpo. A próxima inicialização terá travadas enquanto ele é recriado.',
        'Shortcut created on the desktop.': 'Atalho criado na área de trabalho.',
        'Shown where the game uses the PSN name.': 'Exibido onde o jogo usa o nome da PSN.',
        'Starting the next game of the collection…': 'Iniciando o próximo jogo da coleção…',
        'Steam did not close. Close it yourself and try again.': 'A Steam não fechou. Feche-a e tente novamente.',
        'Steam has to close while the game is added; it starts again afterwards. Close Steam now?':
        'A Steam precisa fechar enquanto o jogo é adicionado; ela abrirá novamente depois. Fechar a Steam agora?',
        'Steam was not found on this PC (or nobody has signed in to it yet).':
        'A Steam não foi encontrada neste PC (ou ninguém iniciou sessão ainda).',
        "Temporal anti-aliasing at the games' 1920 × 1080, with their own motion vectors: smoother edges and less shimmering than the games' own. DLSS needs a GeForce RTX; FSR 4 runs best on a Radeon RX 7000/9000 or a GeForce RTX 40/50.":
        'Antisserrilhamento temporal em 1920 × 1080 com os vetores de movimento dos jogos: bordas mais suaves e menos cintilação que o original. DLSS requer uma GeForce RTX; FSR 4 funciona melhor em Radeon RX 7000/9000 ou GeForce RTX 40/50.',
        'The collection is in your Steam library. It starts with the settings saved in this launcher.':
        'A coleção está na sua biblioteca Steam. Ela será iniciada com as configurações salvas neste launcher.',
        'The cursor stays in the game window; hold Left Alt (or switch windows) to free it. Insert opens the in-game settings menu.':
        'O cursor permanece na janela do jogo; segure Alt esquerdo (ou troque de janela) para liberá-lo. Insert abre as configurações dentro do jogo.',
        'The folder with eboot.bin, big2-ps4_Shipping.elf and big3-ps4_Shipping.elf.':
        'A pasta com eboot.bin, big2-ps4_Shipping.elf e big3-ps4_Shipping.elf.',
        'The game stopped: {}': 'O jogo parou: {}',
        'The games render at 1920 × 1080. A larger window, or fullscreen on a 1440p or 4K screen, gets the image upscaled to fit.':
        'Os jogos renderizam em 1920 × 1080. Em uma janela maior ou em tela cheia a 1440p ou 4K, a imagem é ampliada para caber.',
        'The language the PS4 would be set to; the games use it for text and speech.':
        'O idioma que estaria configurado no PS4; os jogos o usam para textos e vozes.',
        'This game is not in the chosen folder.': 'Este jogo não está na pasta escolhida.',
        'Update': 'Atualizar', 'Update failed: {}': 'A atualização falhou: {}',
        'Version {} is available.': 'A versão {} está disponível.', "What's new": 'Novidades',
        'Where saves and the shader cache are kept': 'Onde os jogos salvos e o cache de shaders são mantidos',
        'Which controller button gives each PS4 button (Xbox / PlayStation names).':
        'Qual botão do controle corresponde a cada botão do PS4 (nomes de Xbox / PlayStation).',
        'You have the latest version ({}).': 'Você tem a versão mais recente ({}).',
        'Your display runs at {} Hz.': 'Sua tela funciona a {} Hz.',
        'Your own decrypted dump of the collection (CUSA02343, CUSA02320, …).':
        'Seu próprio dump descriptografado da coleção (CUSA02343, CUSA02320, …).',
        'by {}': 'por {}', 'see the Log page': 'consulte a página Registro',
    },
    'fr': {
        'Automatic (Windows)': 'Automatique (Windows)', 'Play': 'Jouer', 'Stop': 'Arrêter',
        'Game': 'Jeu', 'Display': 'Affichage', 'Controls': 'Commandes', 'Advanced': 'Avancé',
        'Log': 'Journal', 'Library': 'Bibliothèque', 'Double-click a cover to launch':
        'Double-cliquez sur une jaquette pour lancer', 'Game folder': 'Dossier du jeu',
        'The folder with eboot.bin': 'Le dossier contenant eboot.bin',
        'Saves folder': 'Dossier des sauvegardes', 'Console': 'Console', 'Language': 'Langue',
        'Launcher language': 'Langue du launcher', 'Interface language': "Langue de l’interface",
        'This changes only the launcher. The game language is selected on the Game page.':
        'Cela change uniquement le launcher. La langue du jeu se choisit sur la page Jeu.',
        'Player name': 'Nom du joueur', 'Fullscreen': 'Plein écran', 'Presentation': 'Présentation',
        'Frame rate': "Fréquence d’images", 'Resolution': 'Résolution',
        'Window size': 'Taille de la fenêtre', 'Upscaling': 'Mise à l’échelle',
        'Sharpening': 'Netteté', 'Anti-aliasing': 'Anticrénelage',
        'Anti-aliasing sharpness': "Netteté de l’anticrénelage",
        'Frame generation': "Génération d’images", 'Multiplier': 'Multiplicateur',
        'Show the FPS counter': 'Afficher le compteur de FPS', 'Mouse': 'Souris',
        'Look around with the mouse': 'Orienter la caméra avec la souris',
        'Mouse sensitivity': 'Sensibilité de la souris',
        'Invert the mouse vertically': 'Inverser verticalement la souris',
        'Keyboard and mouse buttons': 'Clavier et boutons de la souris',
        'Controller': 'Manette', 'Default keys': 'Touches par défaut',
        'Default controller buttons': 'Boutons de manette par défaut',
        'Maintenance and options for testing.': 'Maintenance et options de test.',
        'Close the launcher when the game starts': 'Fermer le launcher au démarrage du jeu',
        'Environment': 'Environnement', 'Shader cache': 'Cache des shaders', 'Updates': 'Mises à jour',
        'Clear shader cache': 'Vider le cache des shaders',
        'Open the saves folder': 'Ouvrir le dossier des sauvegardes',
        'Look for a new version when the launcher starts': 'Rechercher les mises à jour au démarrage du launcher',
        'Check for updates': 'Rechercher les mises à jour', 'Releases page': 'Page des versions',
        'Browse…': 'Parcourir…', 'Open': 'Ouvrir', 'Add to Steam': 'Ajouter à Steam',
        'Desktop shortcut': 'Raccourci sur le bureau', 'Copy': 'Copier',
        'Choose the game folder (the one with eboot.bin) on the Game page.':
        'Choisissez le dossier du jeu (celui qui contient eboot.bin) sur la page Jeu.',
        'No game folder yet.': "Aucun dossier de jeu n’a encore été choisi.",
        'The game is running.': 'Le jeu est en cours d’exécution.',
        'Native port: the original PS4 game code runs directly on Windows.':
        'Portage natif : le code original du jeu PS4 s’exécute directement sous Windows.',
        'version': 'version', 'version {}': 'version {}',
        '\n— the game exited (code {}) —\n': '\n— le jeu s’est terminé (code {}) —\n',
        'AMD RCAS after FSR 1 (only while the image is enlarged).':
        "AMD RCAS après FSR 1 (uniquement lorsque l’image est agrandie).",
        'Choose the game folder with eboot.bin.': 'Choisissez le dossier du jeu contenant eboot.bin.',
        'Click a box, then press a key or a mouse button (the wheel counts too). ✕ removes a binding. Each action can have two.':
        'Cliquez sur une case, puis appuyez sur une touche ou un bouton de souris (la molette compte aussi). ✕ supprime une attribution. Chaque action peut en avoir deux.',
        'Close the game before updating.': 'Fermez le jeu avant la mise à jour.',
        'Compiled shaders are kept so the game does not stutter again. Clear the cache only when the picture is broken after an update.':
        "Les shaders compilés sont conservés pour éviter de nouvelles saccades. Videz le cache uniquement si l’image présente des défauts après une mise à jour.",
        'Could not add the game to Steam: {}': "Impossible d’ajouter le jeu à Steam : {}",
        'Could not check for updates: {}': 'Impossible de rechercher les mises à jour : {}',
        'Could not install the update. Download it from the releases page.':
        'Impossible d’installer la mise à jour. Téléchargez-la depuis la page des versions.',
        'Could not start: {}': 'Impossible de démarrer : {}',
        'Downloading version {}… {}%': 'Téléchargement de la version {}… {} %',
        'Empty: the user folder next to the launcher.': 'Vide : le dossier user à côté du launcher sera utilisé.',
        'Extra variables for the game, NAME=value separated by spaces.':
        'Variables supplémentaires pour le jeu, NOM=valeur séparées par des espaces.',
        'FSR 1 keeps edges sharp when the image is enlarged; bilinear is softer.':
        "FSR 1 conserve des contours nets lors de l’agrandissement ; le bilinéaire est plus doux.",
        'FSR 3 makes one frame (2×). DLSS makes up to three on a GeForce RTX 50 (4×), one on an RTX 40.':
        'FSR 3 génère une image (2×). DLSS en génère jusqu’à trois sur une GeForce RTX 50 (4×) et une sur une RTX 40.',
        "Frames made between two of the game's, for smoother motion: 60 FPS shows as 120. Use Mailbox or Immediate presentation and a display of at least that refresh rate. Adds a little input latency; menus and cutscene videos are not generated. Use it with the 60 or 120 FPS frame rate: above about 165 FPS it pauses.":
        'Images créées entre deux images du jeu pour un mouvement plus fluide : 60 FPS sont affichées comme 120. Utilisez la présentation Mailbox ou Immédiate et un écran ayant au moins cette fréquence. Cela ajoute un peu de latence ; les menus et les cinématiques ne sont pas générés. Utilisez-la à 60 ou 120 FPS : au-dessus d’environ 165 FPS, elle se met en pause.',
        "Gameplay keeps its normal speed at every setting; the cutscene videos always play at 60 FPS. Above your display's refresh rate use Mailbox or Immediate.":
        'Le jeu conserve sa vitesse normale avec chaque réglage ; les cinématiques sont toujours lues à 60 FPS. Au-dessus de la fréquence de votre écran, utilisez Mailbox ou Immédiate.',
        'How far the camera turns for a mouse movement; 1.00 is the default.':
        'Amplitude de rotation de la caméra selon le mouvement de la souris ; 1.00 est la valeur par défaut.',
        'How the game window is shown.': 'Mode d’affichage de la fenêtre du jeu.',
        'Install version {} now? The launcher closes, installs it and opens again. Saves and settings are kept.':
        'Installer la version {} maintenant ? Le launcher se fermera, installera la version puis se rouvrira. Les sauvegardes et réglages seront conservés.',
        'Invert the right stick vertically': 'Inverser verticalement le stick droit',
        'Keyboard, mouse and controller. They work together; changes apply when the game starts.':
        'Clavier, souris et manette. Ils fonctionnent ensemble ; les changements s’appliquent au démarrage du jeu.',
        'Mailbox shows each frame as soon as it is ready without tearing; FIFO waits for the display.':
        'Mailbox affiche chaque image dès qu’elle est prête, sans déchirement ; FIFO attend l’écran.',
        'Not in this folder: ': 'Absent de ce dossier : ',
        'Preparing the game; it opens in its own window…': 'Préparation du jeu ; il s’ouvrira dans sa propre fenêtre…',
        'Press a key…': 'Appuyez sur une touche…', 'Ready: {}': 'Prêt : {}',
        'Set your game folder on the Game page': 'Choisissez le dossier du jeu sur la page Jeu',
        'Shader cache cleared. The next start stutters for a while as it is rebuilt.':
        'Cache des shaders vidé. Le prochain démarrage aura des saccades pendant sa reconstruction.',
        'Shortcut created on the desktop.': 'Raccourci créé sur le bureau.',
        'Shown where the game uses the PSN name.': 'Affiché lorsque le jeu utilise le nom PSN.',
        'Starting the next game of the collection…': 'Démarrage du prochain jeu de la collection…',
        'Steam did not close. Close it yourself and try again.': 'Steam ne s’est pas fermé. Fermez-le et réessayez.',
        'Steam has to close while the game is added; it starts again afterwards. Close Steam now?':
        'Steam doit se fermer pendant l’ajout du jeu ; il redémarrera ensuite. Fermer Steam maintenant ?',
        'Steam was not found on this PC (or nobody has signed in to it yet).':
        'Steam est introuvable sur ce PC (ou personne ne s’y est encore connecté).',
        "Temporal anti-aliasing at the games' 1920 × 1080, with their own motion vectors: smoother edges and less shimmering than the games' own. DLSS needs a GeForce RTX; FSR 4 runs best on a Radeon RX 7000/9000 or a GeForce RTX 40/50.":
        'Anticrénelage temporel à 1920 × 1080 avec les vecteurs de mouvement des jeux : contours plus doux et moins de scintillement que le réglage d’origine. DLSS nécessite une GeForce RTX ; FSR 4 fonctionne mieux sur Radeon RX 7000/9000 ou GeForce RTX 40/50.',
        'The collection is in your Steam library. It starts with the settings saved in this launcher.':
        'La collection est dans votre bibliothèque Steam. Elle démarrera avec les réglages enregistrés dans ce launcher.',
        'The cursor stays in the game window; hold Left Alt (or switch windows) to free it. Insert opens the in-game settings menu.':
        'Le curseur reste dans la fenêtre du jeu ; maintenez Alt gauche (ou changez de fenêtre) pour le libérer. Insert ouvre les réglages dans le jeu.',
        'The folder with eboot.bin, big2-ps4_Shipping.elf and big3-ps4_Shipping.elf.':
        'Le dossier contenant eboot.bin, big2-ps4_Shipping.elf et big3-ps4_Shipping.elf.',
        'The game stopped: {}': 'Le jeu s’est arrêté : {}',
        'The games render at 1920 × 1080. A larger window, or fullscreen on a 1440p or 4K screen, gets the image upscaled to fit.':
        'Les jeux sont rendus en 1920 × 1080. Dans une fenêtre plus grande ou en plein écran sur un écran 1440p ou 4K, l’image est agrandie pour s’adapter.',
        'The language the PS4 would be set to; the games use it for text and speech.':
        'La langue qui serait configurée sur la PS4 ; les jeux l’utilisent pour les textes et les voix.',
        'This game is not in the chosen folder.': 'Ce jeu ne se trouve pas dans le dossier choisi.',
        'Update': 'Mettre à jour', 'Update failed: {}': 'Échec de la mise à jour : {}',
        'Version {} is available.': 'La version {} est disponible.', "What's new": 'Nouveautés',
        'Where saves and the shader cache are kept': 'Emplacement des sauvegardes et du cache des shaders',
        'Which controller button gives each PS4 button (Xbox / PlayStation names).':
        'Bouton de la manette correspondant à chaque bouton PS4 (noms Xbox / PlayStation).',
        'You have the latest version ({}).': 'Vous avez la dernière version ({}).',
        'Your display runs at {} Hz.': 'Votre écran fonctionne à {} Hz.',
        'Your own decrypted dump of the collection (CUSA02343, CUSA02320, …).':
        'Votre propre dump déchiffré de la collection (CUSA02343, CUSA02320, …).',
        'by {}': 'par {}', 'see the Log page': 'consultez la page Journal',
    },
}

for _code, _translations in EXTRA_TRANSLATIONS.items():
    TRANSLATIONS[_code] = {**shared_translations(_code), **_translations}

# Controller profiles are one feature surface; keep its vocabulary together across languages.
_controller_texts = {
    'Controllers', 'Keyboard and mouse. They work together with the controller; changes apply when the game starts.',
    'Choose the active controller, its button style and how its buttons are mapped.', 'Active controller',
    'Use for the game', 'Automatic uses the first connected controller. Choose one here when several are connected.',
    'Detected profile', 'Profile override',
    'Automatic follows SDL detection. Override it only when a controller reports the wrong family.',
    'Checking controller features…', 'Detect controllers', 'Controller layout',
    'Automatic follows the device used last. Manual choices keep one button style.', 'Controller buttons',
    'Mapping mode', 'Recommended uses SDL positions and locks the assignments. Custom keeps your saved mapping.',
    'Selected controller is not connected', 'Nintendo buttons', 'PlayStation', 'Xbox', 'Nintendo',
    'Steam Deck / Steam', 'Generic', 'Recommended', 'Custom',
    'Connect a controller to identify it',
    'SDL positional layout · saved custom mappings stay untouched',
}
_controller_es = {
    'Controllers': 'Mandos',
    'Keyboard and mouse. They work together with the controller; changes apply when the game starts.':
    'Teclado y ratón. Funcionan junto con el mando; los cambios se aplican al iniciar el juego.',
    'Choose the active controller, its button style and how its buttons are mapped.':
    'Elige el mando activo, su estilo de botones y cómo se asignan.',
    'Active controller': 'Mando activo', 'Use for the game': 'Usar para el juego',
    'Automatic uses the first connected controller. Choose one here when several are connected.':
    'Automático usa el primer mando conectado. Elige uno aquí cuando haya varios.',
    'Detected profile': 'Perfil detectado', 'Profile override': 'Forzar perfil',
    'Automatic follows SDL detection. Override it only when a controller reports the wrong family.':
    'Automático sigue la detección de SDL. Cámbialo solo si el mando informa una familia incorrecta.',
    'Checking controller features…': 'Comprobando funciones del mando…',
    'Detect controllers': 'Detectar mandos', 'Controller layout': 'Distribución del mando',
    'Automatic follows the device used last. Manual choices keep one button style.':
    'Automático sigue el último dispositivo usado. Una opción manual mantiene un solo estilo.',
    'Controller buttons': 'Botones del mando', 'Mapping mode': 'Modo de asignación',
    'Recommended uses SDL positions and locks the assignments. Custom keeps your saved mapping.':
    'Recomendado usa las posiciones de SDL y bloquea las asignaciones. Personalizado conserva tu configuración.',
    'Selected controller is not connected': 'El mando seleccionado no está conectado',
    'Nintendo buttons': 'Botones de Nintendo', 'Generic': 'Genérico', 'Recommended': 'Recomendado',
    'Custom': 'Personalizado',
    'Connect a controller to identify it': 'Conecta un mando para identificarlo',
    'SDL positional layout · saved custom mappings stay untouched':
    'Distribución posicional de SDL · las asignaciones personalizadas no se modifican',
}
_controller_pt = {'Controllers': 'Controles', 'Active controller': 'Controle ativo',
                  'Use for the game': 'Usar no jogo', 'Detected profile': 'Perfil detectado',
                  'Profile override': 'Substituir perfil', 'Detect controllers': 'Detectar controles',
                  'Controller layout': 'Layout do controle', 'Controller buttons': 'Botões do controle',
                  'Mapping mode': 'Modo de mapeamento', 'Nintendo buttons': 'Botões Nintendo',
                  'Generic': 'Genérico', 'Recommended': 'Recomendado', 'Custom': 'Personalizado'}
_controller_fr = {'Controllers': 'Manettes', 'Active controller': 'Manette active',
                  'Use for the game': 'Utiliser pour le jeu', 'Detected profile': 'Profil détecté',
                  'Profile override': 'Forcer le profil', 'Detect controllers': 'Détecter les manettes',
                  'Controller layout': 'Disposition de la manette', 'Controller buttons': 'Boutons de la manette',
                  'Mapping mode': 'Mode d’attribution', 'Nintendo buttons': 'Boutons Nintendo',
                  'Generic': 'Générique', 'Recommended': 'Recommandé', 'Custom': 'Personnalisé'}
_controller_de = {'Controllers': 'Controller', 'Active controller': 'Aktiver Controller',
                  'Use for the game': 'Im Spiel verwenden', 'Detected profile': 'Erkanntes Profil',
                  'Profile override': 'Profil überschreiben', 'Detect controllers': 'Controller erkennen',
                  'Controller layout': 'Controller-Layout', 'Controller buttons': 'Controller-Tasten',
                  'Mapping mode': 'Belegungsmodus', 'Nintendo buttons': 'Nintendo-Tasten',
                  'Generic': 'Generisch', 'Recommended': 'Empfohlen', 'Custom': 'Benutzerdefiniert'}
_controller_it = {'Controllers': 'Controller', 'Active controller': 'Controller attivo',
                  'Use for the game': 'Usa per il gioco', 'Detected profile': 'Profilo rilevato',
                  'Profile override': 'Forza profilo', 'Detect controllers': 'Rileva controller',
                  'Controller layout': 'Schema controller', 'Controller buttons': 'Pulsanti controller',
                  'Mapping mode': 'Modalità mappatura', 'Nintendo buttons': 'Pulsanti Nintendo',
                  'Generic': 'Generico', 'Recommended': 'Consigliato', 'Custom': 'Personalizzato'}
_controller_ja = {'Controllers': 'コントローラー', 'Active controller': '使用するコントローラー',
                  'Use for the game': 'ゲームで使用', 'Detected profile': '検出されたプロファイル',
                  'Profile override': 'プロファイルを上書き', 'Detect controllers': 'コントローラーを検出',
                  'Controller layout': 'コントローラー配置', 'Controller buttons': 'コントローラーボタン',
                  'Mapping mode': 'マッピングモード', 'Nintendo buttons': 'Nintendo ボタン',
                  'Generic': '汎用', 'Recommended': '推奨', 'Custom': 'カスタム'}
for _code, _specific in {'es': _controller_es, 'pt': _controller_pt, 'fr': _controller_fr,
                         'de': _controller_de, 'it': _controller_it, 'ja': _controller_ja}.items():
    TRANSLATIONS[_code].update({text: _specific.get(text, text) for text in _controller_texts})


TRANSLATIONS['es'].update({
    'L1 (variant)': 'L1 (variante)',
    'Automatic': 'Automático',
    'Not assigned': 'Sin asignar',
    'Touchpad': 'Panel táctil',
    'Save name': 'Guardar nombre',
    'Cancel': 'Cancelar',
    'Unapplied changes': 'Cambios sin aplicar',
    'Discard changes': 'Descartar cambios',
    'Saved profiles': 'Perfiles guardados', 'Profile name': 'Nombre del perfil',
    'Saved profile': 'Perfil guardado', 'Profile {}': 'Perfil {}',
    'Create new profile': 'Crear nuevo perfil', 'Save changes': 'Guardar cambios',
    'Profile created: {}': 'Perfil creado: {}',
    'Create a new profile from your current settings. Select an existing profile to load or update it.':
        'Crea un perfil con tus ajustes actuales. Selecciona uno existente para cargarlo o actualizarlo.',
    'New profiles are named Profile 1, Profile 2, Profile 3…':
        'Los nuevos perfiles se llaman Perfil 1, Perfil 2, Perfil 3…',
    'Save profile': 'Guardar perfil', 'Load profile': 'Cargar perfil', 'Delete profile': 'Eliminar perfil',
    'Type a name to save your settings, or select a saved profile and load it.':
        'Escribe un nombre para guardar tus ajustes, o selecciona un perfil guardado y cárgalo.',
    'Enter a profile name (1–64 characters).': 'Escribe un nombre de perfil de 1 a 64 caracteres.',
    'Select a saved profile first.': 'Primero selecciona un perfil guardado.',
    'Replace profile "{}"?': '¿Reemplazar el perfil «{}»?',
    'Delete profile "{}"? Current settings will be kept.':
        '¿Eliminar el perfil «{}»? Se conservarán los ajustes actuales.',
    'Profile saved: {}': 'Perfil guardado: {}', 'Profile deleted: {}': 'Perfil eliminado: {}',
    'Profile loaded: {}. Changes apply on the next game start.':
        'Perfil cargado: {}. Los cambios se aplican al volver a iniciar el juego.',
    'Could not read profiles: {}': 'No se pudieron leer los perfiles: {}',
    'Could not save profile: {}': 'No se pudo guardar el perfil: {}',
    'Could not load profile: {}': 'No se pudo cargar el perfil: {}',
    'Could not delete profile: {}': 'No se pudo eliminar el perfil: {}',
    'Use profile': 'Usar perfil', 'Applied profile': 'Perfil aplicado',
    'No controller connected': 'Sin mando conectado',
    'Automatic · waiting for a controller': 'Automático · esperando un mando',
    'Layout and assignments': 'Distribución y asignaciones', 'Default': 'Predeterminado',
    'Default: original buttons. Custom: edit your buttons.':
        'Predeterminado: botones originales. Personalizado: edita tus botones.',
    'Select a button': 'Selecciona un botón',
    'Choose the game button and the controller button that will activate it.':
        'Elige el botón del juego y con qué botón de tu mando activarlo.',
    'Use as alternate': 'Usar como alternativo',
    'Apply change': 'Aplicar cambio',
    'Either controller button sends the game button. Apply, then restart the game.':
        'Cualquiera de los dos activa el botón del juego. Aplica y reinicia el juego.',
    'In-game icons': 'Iconos del juego', 'Preview': 'Vista previa', 'Behavior': 'Comportamiento',
    'This changes the icons in the game, not your controller assignments.':
        'Cambia los iconos del juego sin modificar las asignaciones del mando.',
    'The chosen icon style stays fixed; assignments stay unchanged.':
        'El estilo elegido permanece fijo y conserva tus asignaciones.',
    'Show all assignments': 'Ver todas las asignaciones', 'Face buttons': 'Botones frontales',
    'Shoulders and triggers': 'Botones superiores y gatillos', 'Sticks and D-pad': 'Sticks y cruceta',
    'Menu and extra buttons': 'Menú y botones adicionales', 'Rear buttons': 'Botones traseros',
    'Visual model': 'Modelo visual',
    'A manual model changes the illustration, not the available buttons.':
        'El modelo manual cambia la ilustración, no los botones disponibles.',
    'Offline preview': 'Vista previa sin conexión',
    'Rear view (turned over)': 'Vista trasera (mando volteado)',
    'Your controller button': 'Botón de tu mando', 'Alternate button (optional)': 'Botón alternativo (opcional)',
    'Game button (PS4)': 'Botón del juego (PS4)', 'Assignment status': 'Estado de las asignaciones',
    'This button is not exposed by the connected controller.':
        'El mando conectado no expone este botón.',
    'Choose Custom to edit assignments.': 'Elige Personalizado para editar las asignaciones.',
    'Assign button': 'Asignar botón', 'Currently assigned: {}': 'Asignación actual: {}',
    'Changes apply the next time the game starts.': 'Los cambios se aplican al volver a iniciar el juego.',
    'Capabilities not checked; offline editing is available.':
        'Capacidades sin comprobar; puedes editar sin conectar un mando.',
    'Not exposed: {}': 'No disponibles: {}', 'Not exposed': 'No disponible',
    'Multiple actions: {}': 'Varias acciones: {}',
})


def translate(language, text):
    return TRANSLATIONS.get(language, {}).get(text, text)


for _language, _texts in {
    'es': ('Botones de GameCube', 'Clic del gatillo L', 'Clic del gatillo R',
           'El mando GameCube original tiene menos botones. Usa Personalizado o el teclado para las acciones que faltan.'),
    'pt': ('Botões do GameCube', 'Clique do gatilho L', 'Clique do gatilho R',
           'O controle GameCube original tem menos botões. Use Personalizado ou o teclado para as ações restantes.'),
    'fr': ('Boutons GameCube', 'Clic de la gâchette L', 'Clic de la gâchette R',
           'La manette GameCube originale a moins de boutons. Utilisez Personnalisé ou le clavier pour les actions manquantes.'),
    'de': ('GameCube-Tasten', 'L-Trigger-Klick', 'R-Trigger-Klick',
           'Der ursprüngliche GameCube-Controller hat weniger Tasten. Nutze eigene Belegungen oder die Tastatur für fehlende Aktionen.'),
    'it': ('Pulsanti GameCube', 'Clic del grilletto L', 'Clic del grilletto R',
           'Il controller GameCube originale ha meno pulsanti. Usa Personalizzato o la tastiera per le azioni mancanti.'),
    'ja': ('GameCubeのボタン', 'Lトリガーの押し込み', 'Rトリガーの押し込み',
           'オリジナルのGameCubeコントローラーはボタン数が少ないため、不足する操作にはカスタム設定かキーボードを使用してください。'),
}.items():
    TRANSLATIONS[_language].update(zip((
        'GameCube buttons', 'L trigger click', 'R trigger click',
        'The original GameCube controller has fewer buttons. Use Custom or the keyboard for missing game actions.'), _texts))


TRANSLATIONS['es'].update({
    'Rename profile': 'Renombrar perfil',
    'New profile name:': 'Nuevo nombre del perfil:',
    'Use a name between 1 and 80 characters.': 'Usa un nombre de 1 a 80 caracteres.',
    'A profile with that name already exists.': 'Ya existe un perfil con ese nombre.',
    'Unapplied changes: {}. Select each game button to apply.':
        'Sin aplicar: {}. Selecciona cada botón del juego para aplicar el cambio.',
    'Discard unapplied button changes and continue?': '¿Descartar los cambios de botones sin aplicar y continuar?',
    'Test buttons': 'Probar botones',
    'Stop button test': 'Detener prueba',
    'Press buttons for 10 seconds. Blue: pressed · Gold: selected.':
        'Pulsa botones durante 10 segundos. Azul: pulsado · Dorado: seleccionado.',
    'Button test stopped.': 'Prueba de botones detenida.',
    'Button test completed.': 'Prueba de botones completada.',
    'Button test timed out.': 'La prueba de botones agotó el tiempo de espera.',
    'No button data received. Connect a controller and try again.':
        'No se recibieron pulsaciones del mando. Conecta un mando y vuelve a probar.',
    'Button test failed: {}': 'No se pudo probar el mando: {}',
    'Selected controller is not connected': 'El mando seleccionado no está conectado',
    'Controller disconnected': 'Mando desconectado',
})


TRANSLATIONS['pt'].update({
    'A manual model changes the illustration, not the available buttons.': 'Um modelo manual muda a ilustração, não os botões disponíveis.',
    'A profile with that name already exists.': 'Já existe um perfil com esse nome.',
    'Applied profile': 'Perfil aplicado',
    'Apply change': 'Aplicar alteração',
    'Assignment status': 'Estado das atribuições',
    'Automatic': 'Automático',
    'Automatic · waiting for a controller': 'Automático · aguardando um controle',
    'Behavior': 'Comportamento',
    'Button test completed.': 'Teste de botões concluído.',
    'Button test failed: {}': 'Falha no teste de botões: {}',
    'Button test stopped.': 'Teste de botões interrompido.',
    'Button test timed out.': 'O teste de botões excedeu o tempo limite.',
    'Capabilities not checked; offline editing is available.': 'Recursos não verificados; edição offline disponível.',
    'Changes apply the next time the game starts.': 'As alterações serão aplicadas na próxima inicialização do jogo.',
    'Choose Custom to edit assignments.': 'Escolha Personalizado para editar as atribuições.',
    'Choose the game button and the controller button that will activate it.': 'Escolha o botão do jogo e o botão do controle que o ativará.',
    'Could not delete profile: {}': 'Não foi possível excluir o perfil: {}',
    'Could not load profile: {}': 'Não foi possível carregar o perfil: {}',
    'Could not read profiles: {}': 'Não foi possível ler os perfis: {}',
    'Could not save profile: {}': 'Não foi possível salvar o perfil: {}',
    'Create a new profile from your current settings. Select an existing profile to load or update it.': 'Crie um perfil com as configurações atuais. Selecione um existente para carregar ou atualizar.',
    'Currently assigned: {}': 'Atribuição atual: {}',
    'Default: original buttons. Custom: edit your buttons.': 'Padrão: botões originais. Personalizado: edite seus botões.',
    'Delete profile': 'Excluir perfil',
    'Delete profile "{}"? Current settings will be kept.': 'Excluir o perfil "{}"? As configurações atuais serão mantidas.',
    'Discard changes': 'Descartar alterações',
    'Discard unapplied button changes and continue?': 'Descartar as alterações de botões não aplicadas e continuar?',
    'Either controller button sends the game button. Apply, then restart the game.': 'Qualquer um dos dois botões ativa o botão do jogo. Aplique e reinicie o jogo.',
    'Game button (PS4)': 'Botão do jogo (PS4)',
    'In-game icons': 'Ícones do jogo',
    'Layout and assignments': 'Layout e atribuições',
    'Multiple actions: {}': 'Várias ações: {}',
    'New profile name:': 'Novo nome do perfil:',
    'New profiles are named Profile 1, Profile 2, Profile 3…': 'Novos perfis são chamados Perfil 1, Perfil 2, Perfil 3…',
    'No button data received. Connect a controller and try again.': 'Nenhum dado de botões recebido. Conecte um controle e tente novamente.',
    'No controller connected': 'Nenhum controle conectado',
    'Not assigned': 'Não atribuído',
    'Not exposed: {}': 'Não disponíveis: {}',
    'Press buttons for 10 seconds. Blue: pressed · Gold: selected.': 'Pressione botões por 10 segundos. Azul: pressionado · Dourado: selecionado.',
    'Preview': 'Prévia',
    'Profile deleted: {}': 'Perfil excluído: {}',
    'Profile loaded: {}. Changes apply on the next game start.': 'Perfil carregado: {}. As alterações serão aplicadas na próxima inicialização do jogo.',
    'Profile saved: {}': 'Perfil salvo: {}',
    'Profile {}': 'Perfil {}',
    'Rename profile': 'Renomear perfil',
    'Replace profile "{}"?': 'Substituir o perfil "{}"?',
    'Save name': 'Salvar nome',
    'Saved profile': 'Perfil salvo',
    'Saved profiles': 'Perfis salvos',
    'Select a button': 'Selecione um botão',
    'Select a saved profile first.': 'Selecione um perfil salvo primeiro.',
    'Stop button test': 'Parar teste de botões',
    'Test buttons': 'Testar botões',
    'The chosen icon style stays fixed; assignments stay unchanged.': 'O estilo de ícones fica fixo; as atribuições não mudam.',
    'This button is not exposed by the connected controller.': 'Este botão não é disponibilizado pelo controle conectado.',
    'This changes the icons in the game, not your controller assignments.': 'Muda os ícones do jogo, não as atribuições do controle.',
    'Touchpad': 'Touchpad',
    'Unapplied changes': 'Alterações não aplicadas',
    'Unapplied changes: {}. Select each game button to apply.': 'Alterações não aplicadas: {}. Selecione cada botão do jogo para aplicar.',
    'Use a name between 1 and 80 characters.': 'Use um nome entre 1 e 80 caracteres.',
    'Use as alternate': 'Usar como alternativo',
    'Use profile': 'Usar perfil',
    'Visual model': 'Modelo visual',
})


TRANSLATIONS['fr'].update({
    'A manual model changes the illustration, not the available buttons.': 'Un modèle manuel change l’illustration, pas les boutons disponibles.',
    'A profile with that name already exists.': 'Un profil porte déjà ce nom.',
    'Applied profile': 'Profil appliqué',
    'Apply change': 'Appliquer la modification',
    'Assignment status': 'État des attributions',
    'Automatic': 'Automatique',
    'Automatic · waiting for a controller': 'Automatique · en attente d’une manette',
    'Behavior': 'Comportement',
    'Button test completed.': 'Test des boutons terminé.',
    'Button test failed: {}': 'Échec du test des boutons : {}',
    'Button test stopped.': 'Test des boutons arrêté.',
    'Button test timed out.': 'Le test des boutons a dépassé le délai.',
    'Capabilities not checked; offline editing is available.': 'Capacités non vérifiées ; modification hors connexion disponible.',
    'Changes apply the next time the game starts.': 'Les modifications s’appliqueront au prochain lancement du jeu.',
    'Choose Custom to edit assignments.': 'Choisissez Personnalisé pour modifier les attributions.',
    'Choose the game button and the controller button that will activate it.': 'Choisissez le bouton du jeu et celui de votre manette qui l’activera.',
    'Could not delete profile: {}': 'Impossible de supprimer le profil : {}',
    'Could not load profile: {}': 'Impossible de charger le profil : {}',
    'Could not read profiles: {}': 'Impossible de lire les profils : {}',
    'Could not save profile: {}': 'Impossible d’enregistrer le profil : {}',
    'Create a new profile from your current settings. Select an existing profile to load or update it.': 'Créez un profil avec les réglages actuels. Sélectionnez un profil existant pour le charger ou le mettre à jour.',
    'Currently assigned: {}': 'Attribution actuelle : {}',
    'Default: original buttons. Custom: edit your buttons.': 'Par défaut : boutons d’origine. Personnalisé : modifiez vos boutons.',
    'Delete profile': 'Supprimer le profil',
    'Delete profile "{}"? Current settings will be kept.': 'Supprimer le profil « {} » ? Les réglages actuels seront conservés.',
    'Discard changes': 'Abandonner les modifications',
    'Discard unapplied button changes and continue?': 'Abandonner les modifications de boutons non appliquées et continuer ?',
    'Either controller button sends the game button. Apply, then restart the game.': 'L’un ou l’autre bouton active le bouton du jeu. Appliquez, puis relancez le jeu.',
    'Game button (PS4)': 'Bouton du jeu (PS4)',
    'In-game icons': 'Icônes du jeu',
    'Layout and assignments': 'Disposition et attributions',
    'Multiple actions: {}': 'Plusieurs actions : {}',
    'New profile name:': 'Nouveau nom du profil :',
    'New profiles are named Profile 1, Profile 2, Profile 3…': 'Les nouveaux profils s’appellent Profil 1, Profil 2, Profil 3…',
    'No button data received. Connect a controller and try again.': 'Aucune donnée de bouton reçue. Connectez une manette et réessayez.',
    'No controller connected': 'Aucune manette connectée',
    'Not assigned': 'Non attribué',
    'Not exposed: {}': 'Non disponibles : {}',
    'Press buttons for 10 seconds. Blue: pressed · Gold: selected.': 'Appuyez sur les boutons pendant 10 secondes. Bleu : enfoncé · Doré : sélectionné.',
    'Preview': 'Aperçu',
    'Profile deleted: {}': 'Profil supprimé : {}',
    'Profile loaded: {}. Changes apply on the next game start.': 'Profil chargé : {}. Les modifications s’appliqueront au prochain lancement du jeu.',
    'Profile saved: {}': 'Profil enregistré : {}',
    'Profile {}': 'Profil {}',
    'Rename profile': 'Renommer le profil',
    'Replace profile "{}"?': 'Remplacer le profil « {} » ?',
    'Save name': 'Enregistrer le nom',
    'Saved profile': 'Profil enregistré',
    'Saved profiles': 'Profils enregistrés',
    'Select a button': 'Sélectionnez un bouton',
    'Select a saved profile first.': 'Sélectionnez d’abord un profil enregistré.',
    'Stop button test': 'Arrêter le test des boutons',
    'Test buttons': 'Tester les boutons',
    'The chosen icon style stays fixed; assignments stay unchanged.': 'Le style d’icônes reste fixe ; les attributions ne changent pas.',
    'This button is not exposed by the connected controller.': 'Ce bouton n’est pas disponible sur la manette connectée.',
    'This changes the icons in the game, not your controller assignments.': 'Modifie les icônes du jeu, pas les attributions de la manette.',
    'Touchpad': 'Pavé tactile',
    'Unapplied changes': 'Modifications non appliquées',
    'Unapplied changes: {}. Select each game button to apply.': 'Modifications non appliquées : {}. Sélectionnez chaque bouton du jeu pour les appliquer.',
    'Use a name between 1 and 80 characters.': 'Utilisez un nom de 1 à 80 caractères.',
    'Use as alternate': 'Utiliser comme alternative',
    'Use profile': 'Utiliser le profil',
    'Visual model': 'Modèle visuel',
})


TRANSLATIONS['de'].update({
    'A manual model changes the illustration, not the available buttons.': 'Ein manuelles Modell ändert die Abbildung, nicht die verfügbaren Tasten.',
    'A profile with that name already exists.': 'Ein Profil mit diesem Namen existiert bereits.',
    'Applied profile': 'Angewendetes Profil',
    'Apply change': 'Änderung anwenden',
    'Assignment status': 'Belegungsstatus',
    'Automatic': 'Automatisch',
    'Automatic · waiting for a controller': 'Automatisch · warte auf Controller',
    'Behavior': 'Verhalten',
    'Button test completed.': 'Tastentest abgeschlossen.',
    'Button test failed: {}': 'Tastentest fehlgeschlagen: {}',
    'Button test stopped.': 'Tastentest beendet.',
    'Button test timed out.': 'Zeitüberschreitung beim Tastentest.',
    'Capabilities not checked; offline editing is available.': 'Funktionen ungeprüft; Bearbeiten ohne Verbindung möglich.',
    'Changes apply the next time the game starts.': 'Änderungen gelten beim nächsten Spielstart.',
    'Choose Custom to edit assignments.': 'Wähle Benutzerdefiniert zum Bearbeiten der Belegung.',
    'Choose the game button and the controller button that will activate it.': 'Wähle die Spieltaste und die Controller-Taste, die sie auslöst.',
    'Could not delete profile: {}': 'Profil konnte nicht gelöscht werden: {}',
    'Could not load profile: {}': 'Profil konnte nicht geladen werden: {}',
    'Could not read profiles: {}': 'Profile konnten nicht gelesen werden: {}',
    'Could not save profile: {}': 'Profil konnte nicht gespeichert werden: {}',
    'Create a new profile from your current settings. Select an existing profile to load or update it.': 'Erstelle ein Profil mit den aktuellen Einstellungen. Wähle ein vorhandenes zum Laden oder Aktualisieren.',
    'Currently assigned: {}': 'Aktuelle Belegung: {}',
    'Default: original buttons. Custom: edit your buttons.': 'Standard: ursprüngliche Tasten. Benutzerdefiniert: eigene Belegung.',
    'Delete profile': 'Profil löschen',
    'Delete profile "{}"? Current settings will be kept.': 'Profil „{}“ löschen? Die aktuellen Einstellungen bleiben erhalten.',
    'Discard changes': 'Änderungen verwerfen',
    'Discard unapplied button changes and continue?': 'Nicht angewendete Tastenänderungen verwerfen und fortfahren?',
    'Either controller button sends the game button. Apply, then restart the game.': 'Beide Controller-Tasten lösen die Spieltaste aus. Anwenden und das Spiel neu starten.',
    'Game button (PS4)': 'Spieltaste (PS4)',
    'In-game icons': 'Symbole im Spiel',
    'Layout and assignments': 'Anordnung und Belegung',
    'Multiple actions: {}': 'Mehrere Aktionen: {}',
    'New profile name:': 'Neuer Profilname:',
    'New profiles are named Profile 1, Profile 2, Profile 3…': 'Neue Profile heißen Profil 1, Profil 2, Profil 3…',
    'No button data received. Connect a controller and try again.': 'Keine Tastendaten empfangen. Verbinde einen Controller und versuche es erneut.',
    'No controller connected': 'Kein Controller verbunden',
    'Not assigned': 'Nicht belegt',
    'Not exposed: {}': 'Nicht verfügbar: {}',
    'Press buttons for 10 seconds. Blue: pressed · Gold: selected.': 'Drücke 10 Sekunden lang Tasten. Blau: gedrückt · Gold: ausgewählt.',
    'Preview': 'Vorschau',
    'Profile deleted: {}': 'Profil gelöscht: {}',
    'Profile loaded: {}. Changes apply on the next game start.': 'Profil geladen: {}. Änderungen gelten beim nächsten Spielstart.',
    'Profile saved: {}': 'Profil gespeichert: {}',
    'Profile {}': 'Profil {}',
    'Rename profile': 'Profil umbenennen',
    'Replace profile "{}"?': 'Profil „{}“ ersetzen?',
    'Save name': 'Namen speichern',
    'Saved profile': 'Gespeichertes Profil',
    'Saved profiles': 'Gespeicherte Profile',
    'Select a button': 'Taste auswählen',
    'Select a saved profile first.': 'Wähle zuerst ein gespeichertes Profil.',
    'Stop button test': 'Tastentest beenden',
    'Test buttons': 'Tasten testen',
    'The chosen icon style stays fixed; assignments stay unchanged.': 'Der Symbolstil bleibt fest; die Belegung bleibt unverändert.',
    'This button is not exposed by the connected controller.': 'Der verbundene Controller stellt diese Taste nicht bereit.',
    'This changes the icons in the game, not your controller assignments.': 'Ändert die Symbole im Spiel, nicht die Controller-Belegung.',
    'Touchpad': 'Touchpad',
    'Unapplied changes': 'Nicht angewendete Änderungen',
    'Unapplied changes: {}. Select each game button to apply.': 'Nicht angewendete Änderungen: {}. Wähle jede Spieltaste zum Anwenden.',
    'Use a name between 1 and 80 characters.': 'Verwende einen Namen mit 1 bis 80 Zeichen.',
    'Use as alternate': 'Als Alternative verwenden',
    'Use profile': 'Profil verwenden',
    'Visual model': 'Visuelles Modell',
})


TRANSLATIONS['it'].update({
    'A manual model changes the illustration, not the available buttons.': 'Un modello manuale cambia l’illustrazione, non i pulsanti disponibili.',
    'A profile with that name already exists.': 'Esiste già un profilo con questo nome.',
    'Applied profile': 'Profilo applicato',
    'Apply change': 'Applica modifica',
    'Assignment status': 'Stato delle assegnazioni',
    'Automatic': 'Automatico',
    'Automatic · waiting for a controller': 'Automatico · in attesa di un controller',
    'Behavior': 'Comportamento',
    'Button test completed.': 'Test dei pulsanti completato.',
    'Button test failed: {}': 'Test dei pulsanti non riuscito: {}',
    'Button test stopped.': 'Test dei pulsanti interrotto.',
    'Button test timed out.': 'Tempo del test dei pulsanti scaduto.',
    'Capabilities not checked; offline editing is available.': 'Funzionalità non verificate; modifica offline disponibile.',
    'Changes apply the next time the game starts.': 'Le modifiche si applicano al prossimo avvio del gioco.',
    'Choose Custom to edit assignments.': 'Scegli Personalizzato per modificare le assegnazioni.',
    'Choose the game button and the controller button that will activate it.': 'Scegli il pulsante del gioco e quello del controller che lo attiverà.',
    'Could not delete profile: {}': 'Impossibile eliminare il profilo: {}',
    'Could not load profile: {}': 'Impossibile caricare il profilo: {}',
    'Could not read profiles: {}': 'Impossibile leggere i profili: {}',
    'Could not save profile: {}': 'Impossibile salvare il profilo: {}',
    'Create a new profile from your current settings. Select an existing profile to load or update it.': 'Crea un profilo con le impostazioni attuali. Selezionane uno esistente per caricarlo o aggiornarlo.',
    'Currently assigned: {}': 'Assegnazione attuale: {}',
    'Default: original buttons. Custom: edit your buttons.': 'Predefinito: pulsanti originali. Personalizzato: modifica i pulsanti.',
    'Delete profile': 'Elimina profilo',
    'Delete profile "{}"? Current settings will be kept.': 'Eliminare il profilo «{}»? Le impostazioni attuali saranno mantenute.',
    'Discard changes': 'Scarta modifiche',
    'Discard unapplied button changes and continue?': 'Scartare le modifiche ai pulsanti non applicate e continuare?',
    'Either controller button sends the game button. Apply, then restart the game.': 'Entrambi i pulsanti attivano quello del gioco. Applica e riavvia il gioco.',
    'Game button (PS4)': 'Pulsante del gioco (PS4)',
    'In-game icons': 'Icone di gioco',
    'Layout and assignments': 'Schema e assegnazioni',
    'Multiple actions: {}': 'Più azioni: {}',
    'New profile name:': 'Nuovo nome del profilo:',
    'New profiles are named Profile 1, Profile 2, Profile 3…': 'I nuovi profili si chiamano Profilo 1, Profilo 2, Profilo 3…',
    'No button data received. Connect a controller and try again.': 'Nessun dato dei pulsanti ricevuto. Collega un controller e riprova.',
    'No controller connected': 'Nessun controller collegato',
    'Not assigned': 'Non assegnato',
    'Not exposed: {}': 'Non disponibili: {}',
    'Press buttons for 10 seconds. Blue: pressed · Gold: selected.': 'Premi i pulsanti per 10 secondi. Blu: premuto · Oro: selezionato.',
    'Preview': 'Anteprima',
    'Profile deleted: {}': 'Profilo eliminato: {}',
    'Profile loaded: {}. Changes apply on the next game start.': 'Profilo caricato: {}. Le modifiche si applicano al prossimo avvio del gioco.',
    'Profile saved: {}': 'Profilo salvato: {}',
    'Profile {}': 'Profilo {}',
    'Rename profile': 'Rinomina profilo',
    'Replace profile "{}"?': 'Sostituire il profilo «{}»?',
    'Save name': 'Salva nome',
    'Saved profile': 'Profilo salvato',
    'Saved profiles': 'Profili salvati',
    'Select a button': 'Seleziona un pulsante',
    'Select a saved profile first.': 'Seleziona prima un profilo salvato.',
    'Stop button test': 'Interrompi test dei pulsanti',
    'Test buttons': 'Prova pulsanti',
    'The chosen icon style stays fixed; assignments stay unchanged.': 'Lo stile delle icone rimane fisso; le assegnazioni non cambiano.',
    'This button is not exposed by the connected controller.': 'Questo pulsante non è disponibile sul controller collegato.',
    'This changes the icons in the game, not your controller assignments.': 'Cambia le icone del gioco, non le assegnazioni del controller.',
    'Touchpad': 'Touchpad',
    'Unapplied changes': 'Modifiche non applicate',
    'Unapplied changes: {}. Select each game button to apply.': 'Modifiche non applicate: {}. Seleziona ogni pulsante del gioco per applicarle.',
    'Use a name between 1 and 80 characters.': 'Usa un nome da 1 a 80 caratteri.',
    'Use as alternate': 'Usa come alternativo',
    'Use profile': 'Usa profilo',
    'Visual model': 'Modello visivo',
})


TRANSLATIONS['ja'].update({
    'A manual model changes the illustration, not the available buttons.': '手動モデルはイラストだけを変更し、使用可能なボタンは変更しません。',
    'A profile with that name already exists.': '同じ名前のプロファイルが既にあります。',
    'Applied profile': '適用中のプロファイル',
    'Apply change': '変更を適用',
    'Assignment status': '割り当ての状態',
    'Automatic': '自動',
    'Automatic · waiting for a controller': '自動 · コントローラーの接続待ち',
    'Behavior': '動作',
    'Button test completed.': 'ボタンテストが完了しました。',
    'Button test failed: {}': 'ボタンテストに失敗しました: {}',
    'Button test stopped.': 'ボタンテストを停止しました。',
    'Button test timed out.': 'ボタンテストがタイムアウトしました。',
    'Capabilities not checked; offline editing is available.': '機能は未確認です。未接続でも編集できます。',
    'Changes apply the next time the game starts.': '変更は次回のゲーム起動時に適用されます。',
    'Choose Custom to edit assignments.': '割り当てを編集するにはカスタムを選択してください。',
    'Choose the game button and the controller button that will activate it.': 'ゲーム内のボタンと、それを操作するコントローラーのボタンを選んでください。',
    'Could not delete profile: {}': 'プロファイルを削除できませんでした: {}',
    'Could not load profile: {}': 'プロファイルを読み込めませんでした: {}',
    'Could not read profiles: {}': 'プロファイル一覧を読み込めませんでした: {}',
    'Could not save profile: {}': 'プロファイルを保存できませんでした: {}',
    'Create a new profile from your current settings. Select an existing profile to load or update it.': '現在の設定から新しいプロファイルを作成します。既存のプロファイルを選ぶと読み込みや更新ができます。',
    'Currently assigned: {}': '現在の割り当て: {}',
    'Default: original buttons. Custom: edit your buttons.': '標準: 元のボタン配置。カスタム: ボタンを編集。',
    'Delete profile': 'プロファイルを削除',
    'Delete profile "{}"? Current settings will be kept.': 'プロファイル「{}」を削除しますか？現在の設定は保持されます。',
    'Discard changes': '変更を破棄',
    'Discard unapplied button changes and continue?': '未適用のボタン変更を破棄して続行しますか？',
    'Either controller button sends the game button. Apply, then restart the game.': 'どちらのボタンでもゲーム内のボタンを操作できます。適用後、ゲームを再起動してください。',
    'Game button (PS4)': 'ゲーム内のボタン（PS4）',
    'In-game icons': 'ゲーム内アイコン',
    'Layout and assignments': '配置と割り当て',
    'Multiple actions: {}': '複数の操作: {}',
    'New profile name:': '新しいプロファイル名:',
    'New profiles are named Profile 1, Profile 2, Profile 3…': '新しいプロファイルはプロファイル1、プロファイル2、プロファイル3…と命名されます。',
    'No button data received. Connect a controller and try again.': 'ボタンの入力がありません。コントローラーを接続して再試行してください。',
    'No controller connected': 'コントローラー未接続',
    'Not assigned': '未割り当て',
    'Not exposed: {}': '使用不可: {}',
    'Press buttons for 10 seconds. Blue: pressed · Gold: selected.': '10秒間ボタンを押してください。青: 入力中 · 金: 選択中。',
    'Preview': 'プレビュー',
    'Profile deleted: {}': 'プロファイルを削除しました: {}',
    'Profile loaded: {}. Changes apply on the next game start.': 'プロファイルを読み込みました: {}。変更は次回のゲーム起動時に適用されます。',
    'Profile saved: {}': 'プロファイルを保存しました: {}',
    'Profile {}': 'プロファイル{}',
    'Rename profile': 'プロファイル名を変更',
    'Replace profile "{}"?': 'プロファイル「{}」を置き換えますか？',
    'Save name': '名前を保存',
    'Saved profile': '保存済みプロファイル',
    'Saved profiles': '保存済みプロファイル一覧',
    'Select a button': 'ボタンを選択',
    'Select a saved profile first.': '最初に保存済みプロファイルを選択してください。',
    'Stop button test': 'ボタンテストを停止',
    'Test buttons': 'ボタンをテスト',
    'The chosen icon style stays fixed; assignments stay unchanged.': 'アイコンのスタイルを固定します。割り当ては変更されません。',
    'This button is not exposed by the connected controller.': '接続中のコントローラーではこのボタンを使用できません。',
    'This changes the icons in the game, not your controller assignments.': 'ゲーム内アイコンのみを変更し、コントローラーの割り当ては変更しません。',
    'Touchpad': 'タッチパッド',
    'Unapplied changes': '未適用の変更',
    'Unapplied changes: {}. Select each game button to apply.': '未適用の変更: {}。各ゲーム内ボタンを選んで適用してください。',
    'Use a name between 1 and 80 characters.': '名前は1〜80文字で入力してください。',
    'Use as alternate': '代替ボタンに設定',
    'Use profile': '使用するプロファイル',
    'Visual model': '表示モデル',
})


TRANSLATIONS['pt'].update({
    'Alternate button (optional)': 'Botão alternativo (opcional)',
    'Cancel': 'Cancelar',
    'Create new profile': 'Criar novo perfil',
    'Default': 'Padrão',
    'Face buttons': 'Botões frontais',
    'L1 (variant)': 'L1 (variante)',
    'Load profile': 'Carregar perfil',
    'Menu and extra buttons': 'Menu e botões extras',
    'Not exposed': 'Não disponível',
    'Offline preview': 'Prévia offline',
    'Profile created: {}': 'Perfil criado: {}',
    'Rear buttons': 'Botões traseiros',
    'Rear view (turned over)': 'Vista traseira (virado)',
    'Save changes': 'Salvar alterações',
    'Shoulders and triggers': 'Botões superiores e gatilhos',
    'Show all assignments': 'Mostrar todas as atribuições',
    'Sticks and D-pad': 'Sticks e direcional',
    'Your controller button': 'Botão do seu controle',
})


TRANSLATIONS['fr'].update({
    'Alternate button (optional)': 'Bouton alternatif (facultatif)',
    'Cancel': 'Annuler',
    'Create new profile': 'Créer un profil',
    'Default': 'Par défaut',
    'Face buttons': 'Boutons de façade',
    'L1 (variant)': 'L1 (variante)',
    'Load profile': 'Charger le profil',
    'Menu and extra buttons': 'Menu et boutons supplémentaires',
    'Not exposed': 'Non disponible',
    'Offline preview': 'Aperçu hors connexion',
    'Profile created: {}': 'Profil créé : {}',
    'Rear buttons': 'Boutons arrière',
    'Rear view (turned over)': 'Vue arrière (manette retournée)',
    'Save changes': 'Enregistrer les modifications',
    'Shoulders and triggers': 'Boutons supérieurs et gâchettes',
    'Show all assignments': 'Afficher toutes les attributions',
    'Sticks and D-pad': 'Sticks et croix directionnelle',
    'Your controller button': 'Bouton de votre manette',
})


TRANSLATIONS['de'].update({
    'Alternate button (optional)': 'Alternative Taste (optional)',
    'Cancel': 'Abbrechen',
    'Create new profile': 'Neues Profil erstellen',
    'Default': 'Standard',
    'Face buttons': 'Fronttasten',
    'L1 (variant)': 'L1 (Variante)',
    'Load profile': 'Profil laden',
    'Menu and extra buttons': 'Menü- und Zusatztasten',
    'Not exposed': 'Nicht verfügbar',
    'Offline preview': 'Vorschau ohne Verbindung',
    'Profile created: {}': 'Profil erstellt: {}',
    'Rear buttons': 'Rückseitige Tasten',
    'Rear view (turned over)': 'Rückansicht (umgedreht)',
    'Save changes': 'Änderungen speichern',
    'Shoulders and triggers': 'Schultertasten und Trigger',
    'Show all assignments': 'Alle Belegungen anzeigen',
    'Sticks and D-pad': 'Sticks und Steuerkreuz',
    'Your controller button': 'Taste deines Controllers',
})


TRANSLATIONS['it'].update({
    'Alternate button (optional)': 'Pulsante alternativo (facoltativo)',
    'Cancel': 'Annulla',
    'Create new profile': 'Crea nuovo profilo',
    'Default': 'Predefinito',
    'Face buttons': 'Pulsanti frontali',
    'L1 (variant)': 'L1 (variante)',
    'Load profile': 'Carica profilo',
    'Menu and extra buttons': 'Menu e pulsanti aggiuntivi',
    'Not exposed': 'Non disponibile',
    'Offline preview': 'Anteprima offline',
    'Profile created: {}': 'Profilo creato: {}',
    'Rear buttons': 'Pulsanti posteriori',
    'Rear view (turned over)': 'Vista posteriore (capovolto)',
    'Save changes': 'Salva modifiche',
    'Shoulders and triggers': 'Pulsanti dorsali e grilletti',
    'Show all assignments': 'Mostra tutte le assegnazioni',
    'Sticks and D-pad': 'Stick e croce direzionale',
    'Your controller button': 'Pulsante del tuo controller',
})


TRANSLATIONS['ja'].update({
    'Alternate button (optional)': '代替ボタン（任意）',
    'Cancel': 'キャンセル',
    'Create new profile': '新しいプロファイルを作成',
    'Default': '標準',
    'Face buttons': '前面ボタン',
    'L1 (variant)': 'L1（派生モデル）',
    'Load profile': 'プロファイルを読み込む',
    'Menu and extra buttons': 'メニューと追加ボタン',
    'Not exposed': '使用不可',
    'Offline preview': '未接続プレビュー',
    'Profile created: {}': 'プロファイルを作成しました: {}',
    'Rear buttons': '背面ボタン',
    'Rear view (turned over)': '背面図（裏返した状態）',
    'Save changes': '変更を保存',
    'Shoulders and triggers': 'ショルダーボタンとトリガー',
    'Show all assignments': 'すべての割り当てを表示',
    'Sticks and D-pad': 'スティックと方向キー',
    'Your controller button': 'コントローラーのボタン',
})
