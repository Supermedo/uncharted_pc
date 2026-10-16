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


def translate(language, text):
    return TRANSLATIONS.get(language, {}).get(text, text)
