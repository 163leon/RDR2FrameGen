# RDR2FrameGen — заметки разработки

Рабочий журнал разработки: что выяснено про игру, Streamline и данные для DLSS-G, в порядке вех. Пути и версии - с машины, на которой мод писался.

## Окружение

- Игра: `E:\SteamLibrary\steamapps\common\Red Dead Redemption 2`, RDR2.exe 1.0.1491.50, Vulkan, borderless 2560×1440 @ 360 Гц.
- RTX 4070, драйвер 621.24, DLSS 310.9.16 (`nvngx_dlss.dll` в папке игры), Reflex в игре = On.
- ASI-загрузчик `dinput8.dll`, ScriptHookRDR2 V1 1.0.1491.17.
- Сборка: VS BuildTools 2022 (`C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools`), CMake, Vulkan SDK `C:\VulkanSDK\1.4.363.0`. RenderDoc не установлен.
- Streamline SDK 2.14.16 (официальный релиз `NVIDIA-RTX/Streamline`), `tools\fetch-streamline.ps1` распаковывает его в `external\streamline\`.

## Как Streamline подключается к Vulkan (ProgrammingGuideManualHooking.md)

- `slInit` — до создания Vulkan-устройства, с флагом `PreferenceFlag::eUseManualHooking`.
- `sl.interposer.dll` экспортирует свои `vkGetInstanceProcAddr` / `vkGetDeviceProcAddr`. Они отдают адреса из `vulkan-1.dll` для всего API, кроме перехватываемых функций.
- Обязательные перехваты (`sl_hooks.h`): `vkQueuePresentKHR`, `vkCreateSwapchainKHR`, `vkDestroySwapchainKHR`, `vkGetSwapchainImagesKHR`, `vkAcquireNextImageKHR`, `vkDeviceWaitIdle`, `vkCreateWin32SurfaceKHR`, `vkDestroySurfaceKHR`.
- Необязательные, но упрощающие всё: `vkCreateInstance` и `vkCreateDevice` через прокси interposer — тогда Streamline сам добавляет расширения, возможности устройства и дополнительные очереди, и `slSetVulkanInfo` вызывать не нужно.
- Если создавать устройство самим: `slGetFeatureRequirements(kFeatureDLSS_G)` → добавить расширения, features 1.2/1.3, очереди (graphics, compute, optical flow: первая очередь первого семейства с optical flow, индекс 0, семейство не должно совпадать с остальными), затем `slSetVulkanInfo`.
- Известная проблема interposer 2.14.1 (и, по заметкам автора мода 10997, 2.14.16): при запросах RDR2 (семейство 0 ×1, семейство 2 ×8, семейство 1 ×2) прокси `vkCreateDevice` дублирует семейство 2 и превышает его ёмкость. Значит, либо свой расчёт очередей + `slSetVulkanInfo`, либо пересборка interposer с исправлением. Проверить на 2.14.16 своими глазами.

### Ошибка очередей в interposer — подтверждена по исходникам 2.14.16

`external\streamline\source\core\sl.interposer\vulkan\wrapper.cpp`, строки 869–948:
- `computeQueueFamily` = последнее семейство «только compute» (на NVIDIA это семейство 2, 8 очередей).
- RDR2 сама запрашивает все 8 очередей семейства 2. Проверка ёмкости не проходит → `continue`, счётчик `extraComputeQueues` остаётся ненулевым.
- Дальше условие `extraComputeQueues > 0 && computeQueueIndex == 0` добавляет **второй** `VkDeviceQueueCreateInfo` для того же семейства 2 — это запрещено Vulkan (семейство должно быть уникальным, очередей больше, чем есть).

Решение для нашего мода: прокси `vkCreateDevice` из interposer не использовать. Перехватываем `vkCreateDevice` сами:
1. `slGetFeatureRequirements(kFeatureDLSS_G / kFeatureReflex / kFeaturePCL)` → расширения устройства, features 1.2/1.3, число очередей.
2. Дописываем в `VkDeviceCreateInfo` игры: дополнительные очереди Streamline в семействе 0 (graphics, умеет и compute, 16 очередей) после очередей игры; очередь optical flow — индекс 0 первого семейства с `VK_QUEUE_OPTICAL_FLOW_BIT_NV`.
3. Вызываем настоящий `vkCreateDevice`, затем `slSetVulkanInfo` (`sl_helpers_vk.h`, `VulkanInfo` v3: `device`, `instance`, `physicalDevice`, `computeQueueIndex/Family`, `graphicsQueueIndex/Family`, `opticalFlowQueueIndex/Family`, `useNativeOpticalFlowMode`, флаги создания очередей).

Пересобирать interposer не нужно, используем подписанные библиотеки NVIDIA как есть.

### Состав SDK

- `bin\x64\` — подписанные production-библиотеки: `sl.interposer.dll`, `sl.common.dll`, `sl.dlss_g.dll`, `sl.reflex.dll`, `sl.pcl.dll`, `nvngx_dlssg.dll` (310.9.16), `NvLowLatencyVk.dll`.
- `bin\x64\development\` — те же плюс `sl.imgui.dll` (отладочный оверлей с визуализацией входных буферов и матриц).
- `include\` — `sl.h`, `sl_consts.h`, `sl_dlss_g.h`, `sl_reflex.h`, `sl_pcl.h`, `sl_hooks.h`, `sl_helpers_vk.h`, `sl_matrix_helpers.h`.
- `source\` — исходники interposer и плагинов (для чтения).

## Веха 2 — что выяснилось на живой игре (15.11.2026)

Итог: игра создаёт instance, устройство и swapchain через Streamline, работает в меню, `slDLSSGGetState` → `status=0`, `numFramesToGenerateMax=1` (то есть 2x). Генерация пока выключена.

Выбранная схема (`fg\src\vk_route.cpp`): перехват экспортов `vulkan-1.dll` через MinHook, вызовы различаются по адресу возврата (из RDR2.exe → прокси Streamline, из Streamline и всего остального → настоящий загрузчик). Игра получает функции и через `vkGetInstanceProcAddr`/`vkGetDeviceProcAddr`, поэтому для нужных имён возвращаем те же хуки.

Три проблемы и их решения:
1. **Очереди.** Вместо своего создания устройства используем прокси `vkCreateDevice`, но Streamline получает от нас «исправленный» список семейств очередей: у семейств «только compute» скрыт бит compute (подмена `vkGetPhysicalDeviceQueueFamilyProperties[2]` только для вызовов из Streamline). В логе: игра просит `family0 x1, family2 x8, family1 x2`, Streamline создаёт `family0 x4, family2 x8, family1 x2, family5 x1` — корректно.
2. **Нулевой instance.** RDR2 не вызывает `vkEnumeratePhysicalDevices` так, чтобы это увидел прокси, и Streamline не знал, какому instance принадлежит устройство → падение в `sl.common` (`m_idt.GetPhysicalDeviceMemoryProperties` = null). Решение: перед `vkCreateDevice` сами вызываем прокси `vkEnumeratePhysicalDevices`.
3. **`vkAcquireNextImage2KHR`.** Игра берёт кадры этой функцией (Vulkan 1.1), Streamline её не перехватывает → игра забирала настоящие кадры мимо подменённого swapchain, Streamline зависал на 2 с, игра падала. Решение: свой хук, который вызывает прокси `vkAcquireNextImageKHR`.

Факты об игре: `apiVersion` 1.1.108 (прокси поднимает до 1.3), swapchain 2560×1440, формат 44 (`B8G8R8A8_UNORM`), `minImageCount=2`, режим MAILBOX; present идёт из отдельного потока; у present один ожидаемый семафор. Игра также запрашивает `vkAcquireFullScreenExclusiveModeEXT` и `vkGetDeviceGroupPresentCapabilitiesKHR`.

Предупреждения Streamline, которые ожидаемы при ручном перехвате: `Hook sl.common:Vulkan:CmdBindPipeline / CmdBindDescriptorSets / BeginCommandBuffer is NOT supported` (нужны только для отслеживания состояния командных буферов, которое при `eUseManualHooking` на нас).

Инструменты отладки: `fg\src\crash_log.cpp` пишет фатальные исключения со стеком в `RDR2FrameGen.log`; `fg\tools\symbolize.cpp` разбирает адреса по PDB (PDB из SDK не совпадают с production-библиотеками, надёжнее `dumpbin /disasm`).

## Что DLSS-G требует на входе (ProgrammingGuideDLSS_G.md)

- Обязательно: глубина, векторы движения (плотные, с движением камеры), общие константы на каждый кадр (`slSetConstants`), Reflex через Streamline с метками кадра (PCL markers), совпадение номера кадра у констант и у `ePresentStart`/`ePresentEnd`.
- «Критично для качества»: картинка без интерфейса (`kBufferTypeHUDLessColor`, то же цветовое пространство и постобработка, что у финального кадра, размер как у backbuffer) и буфер интерфейса (`kBufferTypeUIAlpha` предпочтительно, либо `UIColorAndAlpha`; формула `Final = UI.rgb + (1 - UI.a) * Hudless`). С обоими можно включить `eEnableUserInterfaceRecomposition`.
- Константы: матрицы row-major без джиттера; `mvecScale` такой, чтобы значения попали в [-1,1] (для пиксельных векторов — `1/renderWidth, 1/renderHeight`); `depthInverted`, `cameraMotionIncluded`, `motionVectorsJittered`, `reset`; `renderingGameFrames`.
- Не вызывать `slSetConstants` дважды за кадр.
- Vulkan: игра должна сигналить семафор present при последнем submit и ждать семафор acquire перед новым кадром (раздел 16.0) — проверить, что RDR2 так делает.
- Выключать генерацию (`DLSSGMode::eOff`), когда игра не рендерит мир (пауза, загрузка, видео), и перед любыми операциями с окном/swapchain; при недействительных данных — обнулять теги.
- Теги по умолчанию `eValidUntilPresent`; `eOnlyValidNow` (копия) — только для буферов, которые игра переиспользует до present.
- «Any existing Reflex SDK integration that does not use Streamline cannot be used with DLSS-G» — собственный Reflex игры нужно выключить или перехватить.
- Проверка входных данных: плагин `sl.imgui` из development-сборки (визуализация буферов и матриц), раздел 21.0 (Enhanced debug visualization).

## Откуда берём данные в RDR2

- Глубина, векторы, джиттер, масштаб, флаги: вызов DLSS самой игры `NVSDK_NGX_VULKAN_EvaluateFeature` (параметры `Depth`, `MotionVectors`, `Jitter.Offset.X/Y`, `MV.Scale.X/Y`, `Reset`, `DLSS.Render.Subrect.Dimensions.*`, флаги из `DLSS.Feature.Create.Flags`). По логам чужого мода: глубина `D32_SFLOAT_S8_UINT`, векторы `R16G16_SFLOAT`, векторы в разрешении рендера, глубина обратная.
- Камера (нативные функции, `research\natives.json`):
  - `GET_FINAL_RENDERED_CAM_COORD` 0x5352E025EC2B416F
  - `GET_FINAL_RENDERED_CAM_ROT(rotationOrder)` 0x602685BD85DD26CA
  - `GET_FINAL_RENDERED_CAM_FOV` 0x04AF77971E508F6A
  - геттеров near/far нет (есть только `SET_CAM_NEAR_CLIP`/`SET_CAM_FAR_CLIP`) → near/far нужно брать из памяти (матрица проекции вьюпорта) либо восстановить по значениям глубины.
  - `GET_SCREEN_COORD_FROM_WORLD_COORD` 0xCB50D7AFCC8B0EC6 — пригодится для проверки собранной матрицы: наша проекция точки должна совпасть с игровой.
- Картинка без HUD: момент между концом постобработки и первым рисованием интерфейса. Структуру проходов снимем своим же ASI (лог render pass / dynamic rendering при записи в swapchain-картинку) — RenderDoc с лаунчером Rockstar ненадёжен.

## Уроки из чужого кода (мод 10997), которые учитываем

- `slInit` вызывать внутри самого `vkCreateInstance`, а не при разрешении адреса: загрузчик Vulkan держит блокировку, плагины Streamline грузят DLL → взаимная блокировка.
- Перехваченный `vkGetInstanceProcAddr` вызывается и изнутри interposer (в том числе из его рабочих потоков во время `slInit`) → нужен флаг повторного входа, иначе рекурсия.
- Всегда ставить `DLSSGFlags::eRetainResourcesWhenOff`, иначе каждое выключение/включение пересоздаёт ресурсы (кадр ~140 мс).
- Включение/выключение генерации привязывать к тому, вызывала ли игра DLSS в этом кадре, а не ко времени кадра.
- Нужен HAGS (у пользователя включён).

## Веха 3, первый запуск в игре (2026-11-15)

- Игра 147 с шла через Streamline без ошибок и вылетов, но генерация не включилась: мод не увидел ни одного вызова DLSS.
- Причина: в процессе два рантайма NGX. Streamline грузит `_nvngx.dll`, а старый NGX SDK игры грузит `nvngx.dll` (оба из DriverStore). Перехват стоял только на `_nvngx.dll`.
- Исправление (собрано, в игре не проверено): перехват обоих рантаймов, `nvngx.dll` подгружаем сами сразу после создания устройства, чтобы успеть до Init игры; защита от двойного срабатывания, если один рантайм зовёт другой.
- Открытый вопрос: в том запуске `nvngx_dlss.dll` в процесс не был загружен, то есть игра DLSS не создавала вообще. Либо в настройках игры стоял не DLSS, либо Init игры не прошёл из-за того, что NGX уже инициализирован Streamline (в sl.log после Init игры идёт строка FrameGeneration Shutdown). Добавлен лог Init / Shutdown / GetCapabilityParameters с результатами, чтобы это увидеть.
- Второй запуск: перехват `nvngx.dll` работает. Init игры (Init_Ext, appId 0x60E8AB7) возвращает успех, но `SuperSampling.Available=0`, initResult=0xBAD00004 (FeatureNotFound); игра зовёт Shutdown и убирает DLSS из меню. Причина: ядро NGX общее на оба рантайма, первым его инициализирует Streamline с путём папки RDR2FrameGen, а `nvngx_dlss.dll` там нет (sl.log: "nvngx_dlss.dll doesn't exist in embedded search path"). Исправление: deploy.ps1 кладёт копию `nvngx_dlss.dll` игры в папку RDR2FrameGen. В игре не проверено. При обновлении DLSS игры копию обновлять тоже.
- Третий запуск: DLSS вернулся (available=1). Игра создаёт апскейлер при загрузке сохранения: 1485x835 -> 2560x1440, flags=0x2B (HDR, MVLowRes, DepthInverted, векторы без джиттера), глубина fmt=130 (D32_SFLOAT_S8_UINT), векторы fmt=83 (R16G16_SFLOAT), mvScale 1/1. Через ~30 кадров генерация включилась: `slDLSSGGetState` -> numFramesActuallyPresented=2, status=0, ошибок Streamline нет. Веха 3 выполнена.
- Предупреждения Streamline при включённой генерации: "Invalid backbuffer resource extent" (extent не задан, берётся полный) и "VSync interval 1 not supported with FG" на каждом present (в игре включён VSync).
- У пользователя один вылет через 13 с после включения генерации: AV в главном потоке RDR2.exe+0x75FF57, чтение 0x7FFE9BDD64F8 (адрес в диапазоне DLL, в меню там модуля нет), стек целиком в RDR2.exe. При моём повторе (3 минуты ходьбы) не воспроизвёлся. crash_log теперь пишет регистры, байты кода, VirtualQuery адреса и список выгруженных модулей.

## Веха 4: настоящая камера (2026-11-15)

- `src/camera.cpp`: скрипт ScriptHook каждый тик читает GET_FINAL_RENDERED_CAM_COORD / _ROT (порядок 2) / _FOV; из двух соседних замеров строятся cameraViewToClip, clipToCameraView, clipToPrevClip, prevClipToClip, оси и позиция. Всё в double относительно текущей позиции камеры. FOV в сохранении у лагеря 51.28 (считаем вертикальным).
- Near/far игра не отдаёт: в ini CameraNear=0.15, CameraFar=10000 (допущение, не проверено).
- Синхронизация потоков: если брать самый свежий замер, в ~18% кадров он повторяется или перескакивает через один (замер 0/1/2 тика: 54/492/54). Исправлено: кадры берут замеры строго по порядку, держась на CameraDelay=1 тик позади; после исправления 600/600 кадров по порядку, пересинхронизаций 0.
- Не проверено: совпадает ли выбранный тик с кадром рендера (возможен постоянный сдвиг на кадр) и ориентация оси Y клип-пространства. Проверять development-сборкой Streamline или по картинке.
- В меню паузы скрипт не тикает: мод переходит на нейтральную камеру, генерация остаётся включённой.
- Steam после taskkill RDR2.exe не запускает игру с первого steam://rungameid, нужен второй вызов.

## Вылет RDR2.exe+0x75FF57 (2026-11-15, два раза: через 13 с и через 45 мин после загрузки)

- Оба раза одно место и один адрес. Инструкция `mov r10d,[rax+rdx]`: rdx = глобальная таблица игры RDR2.exe+0x45764F8 (записи по 32 байта, счётчик в +0xEB8), rax = индекс << 5, индекс = 0x3F800000, то есть float 1.0, попавший туда, где ждут целое.
- Поток главный (на нём же идут скрипты). В стеке только RDR2.exe (#04 +0x1428BDA, #05-06/#10 около +0x2B14xxx - похоже на скриптовую VM), кадров нашего мода, Streamline и ScriptHook нет. Первый вылет был ещё до появления нашего скрипта камеры.
- Вывод: скрипт игры вызвал нативную функцию с мусорным индексом. Наш мод скриптовые данные не трогает; под подозрением ASI-моды геймплея, поставленные в эту же сессию и не проверенные долгой игрой (Bandit_Hideouts, Duels, PedDamageOverhaul, RealisticWeather, StashThatLantern, longer_days, whm.smokingBarrels). Не доказано.
- Проверка: длинная игра с Enabled=0 (crash_log теперь ставится и при выключенном моде). Копия лога: build/crash-2026-11-15-run2.log.

## Веха 5: разведка кадра (2026-11-15)

- `src/hud.cpp`: обёртки над функциями устройства, которые игра получает через vkGet*ProcAddr (CreateImageView, CreateFramebuffer, CreateRenderPass, CreateGraphicsPipelines, CmdBeginRenderPass, CmdDraw*, CmdCopy/Blit, QueueSubmit ...). Отслеживает, какие framebuffer ссылаются на картинки swapchain (прокси Streamline, 2 штуки, формат 44 = B8G8R8A8_UNORM).
- В мире (сразу после загрузки) за кадр три прохода в back buffer, динамического рендеринга и вторичных буферов команд нет:
  - A: renderPass с loadOp=CLEAR, 1 вложение, один полноэкранный треугольник (вывод сцены после постобработки);
  - B: loadOp=LOAD, 1 вложение, один полноэкранный треугольник (что это, не выяснено: композит слоя интерфейса или эффект);
  - C: loadOp=LOAD, 2 вложения (с глубиной), рисование 30 вершин + 240 индексов (элементы HUD).
- Порядок записи команд по потокам не равен порядку выполнения; порядок отправки пишет зонд (QueueSubmit).
- Зонд: при DebugLogging=1 клавиша End пишет в лог два кадра (со смешиванием конвейеров и порядком отправки). Нужно снять: в мире с HUD, в меню паузы, на карте.
- План захвата: копия back buffer сразу после прохода A (или после B, если B не интерфейс) в свою картинку, тег kBufferTypeHUDLessColor.

## Веха 5: кадр без интерфейса (2026-11-15, работает)

- Итог разведки (зонд, копия лога build/probe-2026-11-15.log), порядок по QueueSubmit:
  - проход A (CLEAR, 1 вложение, полноэкранный треугольник без смешивания) - готовая сцена. Есть всегда: в мире, в меню паузы, на карте;
  - проход C (LOAD, 2 вложения, смешивание SRC_ALPHA / ONE_MINUS_SRC_ALPHA) - весь интерфейс: HUD, меню паузы, карта. Всегда отправляется после A; в мире без HUD его нет вовсе;
  - проход B (LOAD, 1 вложение, полноэкранный треугольник) отправляется ДО A, то есть A его затирает. Бывает на загрузке и в меню паузы (там три таких подряд), в обычном мире отсутствует. Для захвата не важен.
- Захват: сразу после vkCmdEndRenderPass прохода A (признак: loadOp=CLEAR у вложения swapchain) в тот же буфер команд игры дописывается копия back buffer в нашу картинку (по одной на картинку swapchain, 3 штуки, CONCURRENT на все семейства очередей). Back buffer возвращается в раскладку, в которой его оставил проход (finalLayout = 2, COLOR_ATTACHMENT_OPTIMAL).
- Игра создаёт swapchain с usage=0x14 (без TRANSFER_SRC); мод добавляет TRANSFER_SRC при создании, Streamline и драйвер принимают.
- Тег kBufferTypeHUDLessColor ставится на потоке present перед вызовом present, eValidUntilPresent, без буфера команд, раскладка SHADER_READ_ONLY. Копия берётся по индексу представляемой картинки.
- Проверено: в логе "Frame without interface: handed to DLSS-G", 2 кадра на кадр игры, status 0, новых предупреждений Streamline нет; картинка в мире, в паузе и на карте целая. Не проверено глазами: стало ли меньше размытия миникарты (нужна оценка пользователя); сравнение через HudLess=0 в ini.
- Следствие: карта и меню целиком считаются интерфейсом, DLSS-G их не интерполирует (движение карты идёт с исходной частотой кадров).
- Пользователь подтвердил (2026-11-15): шлейфов на интерфейсе не видит.

## Веха 6: доводка (2026-11-15, собрано, в игру не поставлено)

- Склейка камеры: если за один тик камера ушла дальше 20 м или повернулась больше чем на 60 градусов, кадр идёт без прошлой камеры и с reset=true (катсцены, телепорты, быстрое перемещение). В отладочном логе счётчик cuts. В игре не проверено.
- README.md для мода.
- Осталось: поставить сборку, вернуть DebugLogging=0 в ini игры, проверить Alt+Tab и смену разрешения (пересоздание swapchain с картинками для копии не проверялось), проверить катсцену.

## Вылет RDR2.exe+0x75FF57, третий раз (2026-11-15 20:44) - след ведёт к Realistic Weather

- Тот же адрес, тот же стек до последнего кадра, тот же индекс 0x3F800000 (float 1.0), главный поток. 23 минуты после запуска. Копии: build/crash-2026-11-15-run3.log и ...-run3-RealisticWeather.log.
- Совпадение по времени с логом RealisticWeather.asi в обоих случаях, где время известно:
  - вылет 2: переход FOG -> CLOUDS начат в 16:09:27 на 75 с (конец 16:10:42), вылет в 16:10:45;
  - вылет 3: переход CLOUDS -> SUNNYLA завершён в 20:44:17, вылет в 20:44:21.
  То есть игра падает через 3-4 с после конца погодного перехода, а 1.0 в индексе похоже на «доля перехода = 100%». При этом десятки переходов завершались без вылета, а кадров ASI в стеке нет: падает код игры, которому мод оставил такое состояние.
- Проверка: RealisticWeather.asi переименован в RealisticWeather.asi.off (2026-11-15). Если за 1-2 часа игры вылета нет, причина подтверждена. Вернуть: убрать .off. Тогда же пересмотреть VESTIGIA weatherGraph (он выставлен в расчёте на то, что переходами управляет Realistic Weather).
- Сборка вехи 6 (сброс на склейках камеры) поставлена в игру вместе с этим.

## Вылет RDR2.exe+0x75FF57, четвёртый раз (2026-11-15 21:38) - Realistic Weather и Bandit Hideouts оправданы

- Тот же адрес, стек и индекс. 25 минут после запуска, во время миссии (по логу longer_days миссия началась в 21:32). Копия: build/crash-2026-11-15-run4.log.
- RealisticWeather.asi был выключен, Bandit Hideouts убран (по просьбе пользователя, файлы в <игра>\asi_off\Bandit_Hideouts). Значит, совпадение с погодными переходами было случайным или дело в погоде самой игры. Realistic Weather возвращён.
- Модов, подменяющих скрипты игры (.ysc), в lml нет. Стек - стек главного потока до самого низа (KERNEL32/ntdll), без волокон ScriptHook: команду с мусорным индексом вызывает скрипт самой игры.
- Оставшиеся подозреваемые: ASI (Duels, PedDamageOverhaul, StashThatLantern, longer_days, whm.smokingBarrels, DualSenseRDR2, наш мод) и данные из lml (PDO Reloaded, crime_law, W.E.R.O, VESTIGIA, Better Horses, Online Content Unlocker ...).
- crash_log теперь после стека печатает похожие на имена строки из объектов, на которые указывает стек (поиск имени работающего скрипта), а список выгруженных модулей - только если адрес попал в один из них. Следующий вылет должен назвать скрипт.

## Вылет RDR2.exe+0x75FF57, пятый раз (2026-11-16 19:01) - версия: лишние типы погоды VESTIGIA

- Тот же адрес и стек, 8 минут после запуска, в миссии. Копия: build/crash-2026-11-16-run5.log. Поиск имён скриптов не сработал: стек главного потока кончается раньше окна 0x1800 байт, чтение целиком падало. Исправлено (чтение по 0x100 байт).
- За минуту до вылета цель погоды игры металась CLOUDS <-> SUNNYNO каждые 7-12 с (лог RealisticWeather, строки observed). Вылет 3 случился через 4 с после перехода в SUNNYLA. SUNNYNO/SUNNYLA/SUNNYNYC/SUNNYDENVER/SUNNYSEATTLE - типы погоды, которых нет в игре: их добавляет VESTIGIA (weather.xml, weatherclimates.xml, weatherGraph.xml).
- Версия: скрипт игры получает неизвестный ему тип погоды и передаёт мусор в команду. Объясняет и вылет 4 с выключенным Realistic Weather. Не доказано.
- Проверка: lml\VESTIGIA\active\wea\weatherclimates.xml заменён вариантом самой VESTIGIA «1 - extra weathers removed» (прежний файл рядом: weatherclimates.xml.with-extra-weathers). weatherGraph и weather.xml не тронуты: типы остаются описаны, но климаты их больше не выбирают. В логе RealisticWeather не должно появляться SUNNYxx; если появится и игра упадёт - убирать и из weatherGraph.
- Итог первой сессии после замены (2026-11-16): 2 ч 25 мин без вылета, SUNNYxx в логе погоды нет.

## Веха 6: переключение режима окна (2026-11-17)

- Первый Alt+Enter с включённой генерацией: вылет в потоке Streamline (sl.dlss_g -> sl.common -> SteamOverlayVulkanLayer64 -> SocialClubVulkanLayer, вызов по нулевому указателю). Present шёл из потока Streamline в момент, когда игра пересоздавала окно.
- Исправление: `FramesQuiesce`. Перед Alt+Enter (хук сообщений окна игры), vkCreateSwapchainKHR, vkDestroySwapchainKHR и vkAcquire/ReleaseFullScreenExclusiveModeEXT генерация выключается, вызов ждёт до 300 мс двух present без неё; ещё 30 present после этого генерация не включается. Генерация также выключена, пока окно игры не в фокусе.
- В двух следующих запусках игра после Alt+Enter бесконечно вызывала vkDestroySurfaceKHR с одной и той же поверхностью (лог вырос до 800 МБ, окно не отвечало). В первом из них ожидание стояло и в vkDestroySurfaceKHR. Без мода (Enabled=0) Alt+Enter работает. Причину цикла не нашли: потом он не повторился. Защита: повторное удаление той же поверхности пропускается (в Vulkan это и так недопустимо), на третьем повторе в лог пишутся стеки всех потоков.
- После этого четыре переключения подряд прошли: поверхность, swapchain и копии для кадра без интерфейса пересоздаются, генерация включается сама.
- Генерация теперь выключается на первом же кадре без данных DLSS (раньше на третьем; Streamline в эти кадры писал "Failed to find global tag kBufferTypeDepth"). После короткого сбоя (до 10 кадров) включается через 4 кадра, после долгого - через 30.
- Склейки камеры за 2,5 часа игры: в 122 из 733 отчётов (по 600 кадров) была хотя бы одна, то есть сброс истории срабатывает на катсценах и сменах камеры.
