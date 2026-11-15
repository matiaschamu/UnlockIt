# Guía del proyecto UnlockIt

Este archivo conserva las decisiones y lo aprendido durante el desarrollo. Aplica a todo el proyecto. Actualizarlo cuando cambien el comportamiento, las comprobaciones o sus limitaciones. Las nuevas instrucciones del usuario prevalecen sobre las decisiones aquí registradas.

## Objetivo y preferencias del usuario

- Aplicación nativa de Windows con una ventana simple, sin menús y siempre por encima de las ventanas normales.
- Texto principal: **Arrastre aquí**. Aceptar archivos, carpetas y selecciones múltiples.
- Analizar antes de modificar. Mostrar las rutas de los elementos internos responsables de problemas.
- El análisis y el desbloqueo de carpetas deben incluir todas sus subcarpetas accesibles.
- El usuario pidió una lista de procesos y confirmación independiente para cada uno: elegir cuáles cerrar y cuáles conservar. Se intenta cierre normal y, si sigue reteniendo la selección, la autorización individual también permite finalizarlo, con advertencia de posible pérdida de cambios.
- Si faltan permisos, ofrecer reiniciar como administrador conservando la selección.
- Detectar carpetas internas con marca de solo lectura, restricciones de eliminación y problemas que Windows informe sobre OneDrive.
- Mantener la interfaz y los mensajes en español, con lenguaje sencillo.
- La barra de título debe mostrar `UnlockIt - ver <versión>`. El usuario pidió incremento automático en cada compilación de la aplicación, empezando por `1.0.1`.

## Estructura y tecnología

- `UnlockIt.slnx`: solución de Visual Studio. Incluye la aplicación, no el proyecto de pruebas.
- `UnlockIt.vcxproj` y `.filters`: proyecto C++ y organización del Explorador de soluciones. Registrar los nuevos archivos en ambos cuando corresponda.
- `UnlockIt.cpp`: ventana Win32, dibujo, controles, arrastre, selector de archivos, confirmaciones, trabajo en segundo plano y elevación.
- `UnlockEngine.h` / `UnlockEngine.cpp`: análisis, diagnósticos, consultas de uso, solicitud de cierre y desbloqueo.
- `UnlockIt.rc`, `Resource.h`, iconos y encabezados: recursos y soporte de la plantilla original. Hay recursos de menú heredados, pero la ventana no carga ninguno.
- `UnlockIt-icon.png`: fuente del icono de candado rompiéndose, con transparencia. `build/Convert-Icon.ps1` regenera `UnlockIt.ico` y `small.ico` con diez tamaños de 16 a 256 píxeles, preservando el canal alfa. Los menores de 256 se guardan como DIB de 32 bits, de abajo hacia arriba, con máscara AND alineada a 32 bits; solo 256 usa PNG comprimido, conforme a la recomendación de Microsoft para iconos. Ambos recursos existentes se usan para el ejecutable y la ventana; regenerarlos antes de compilar si cambia el PNG. Ante una imagen antigua en el Explorador, actualizar el archivo con SHChangeNotify sin borrar globalmente la caché ni reiniciar el Explorador.
- `tests/EngineTests.vcxproj` y `tests/EngineTests.cpp`: pruebas independientes del motor.
- `README.md`: guía de funcionamiento, compilación, pruebas y limitaciones. Mantenerla alineada con el código.
- C++20, Unicode y fuentes UTF-8 mediante `/utf-8`. Toolset `v145`, Windows SDK 10.0. Se verificó x64; no dar por probada Win32.
- Bibliotecas de Windows: `Rstrtmgr`, `Advapi32`, `Shell32`, `Ole32` y `Dwmapi`. No requiere un servicio externo propio.
- La carpeta no era un repositorio Git al iniciar este trabajo. Comprobar el estado actual antes de asumir que existe uno.

## Comportamiento que se debe conservar

1. El arrastre utiliza `WM_DROPFILES`, `DragQueryFileW` y `DragFinish`. El clic en la zona abre un selector de archivos con selección múltiple; las carpetas se seleccionan arrastrándolas.
2. La ventana usa `WS_EX_TOPMOST`, no tiene menú ni maximización y admite DPI por monitor. `WS_CLIPCHILDREN` evita pintar encima de los controles hijos. Siempre encima se refiere a ventanas normales, no a las pantallas de seguridad de Windows.
3. Las operaciones usan `std::jthread` y cancelación mediante `stop_token`. El resultado vuelve al hilo de interfaz por mensaje y almacenamiento protegido con mutex. No bloquear la interfaz con recorridos o consultas de procesos.
4. Durante el trabajo se deshabilitan acciones que iniciarían otra operación. Al cerrar se solicita cancelar y se espera a que termine el trabajo.
5. **Desbloquear** pide confirmación para atributos y luego por cada proceso (nombre y PID): Sí autoriza ese cierre y posible finalización; No lo conserva; Cancelar descarta toda la operación antes de efectuar cambios o cierres. No elimina la selección.
6. Conservar el contenido y los otros atributos. Después de desbloquear se vuelve a analizar para mostrar lo que sigue pendiente. Una solicitud de cierre o un cambio de atributo no garantizan la solución.
7. **Volver a analizar** comprueba nuevamente la misma selección.
8. Si se detecta falta de permisos, ofrecer elevar una vez por selección y mantener un botón para hacerlo después. El reinicio usa `ShellExecuteExW` con `runas` y argumentos correctamente entrecomillados. Cancelar UAC conserva la instancia original.
9. La instancia elevada vuelve a analizar. No desbloquea automáticamente: el usuario debe pulsar el botón nuevamente.

## Lo aprendido sobre Windows

- **Solo lectura y permisos son distintos.** `FILE_ATTRIBUTE_READONLY` puede impedir escribir o borrar un archivo. Windows no aplica esa restricción del mismo modo a directorios: una carpeta marcada así no demuestra la causa de un fallo de borrado.
- Las carpetas marcadas se guardan en `Report::readOnlyFolders`, se muestran con una explicación y se les puede quitar la marca recursivamente. `Report::readOnly` cuenta archivos, no carpetas.
- Para eliminar puede bastar `DELETE` sobre el elemento o `FILE_DELETE_CHILD` en su carpeta contenedora. No diagnosticar una restricción definitiva comprobando solo una opción.
- `CheckDeletion` solicita acceso sin `FILE_FLAG_DELETE_ON_CLOSE` y sin borrar. Distingue acceso denegado y bloqueos de uso. En archivos de solo lectura evita interpretar automáticamente el rechazo de `DELETE` como falta de permisos de seguridad.
- Se comprueban permisos de atributos por separado. Poder leer o listar no garantiza poder modificar o eliminar.
- El recorrido usa una pila, rutas Unicode extendidas `\\?\` y UNC, y deduplicación sin distinguir mayúsculas.
- Restart Manager consulta archivos en grupos de hasta 64. Si hay procesos, se consulta por archivo para asociar rutas internas y responsables. No registrar directorios como si fueran archivos.
- Cuando una carpeta devuelve un bloqueo de eliminación, se intenta una consulta adicional de procesos con `NtQueryInformationFile`, clase 47 (`FileProcessIdsUsingFileInformation`). Se resuelve dinámicamente: Microsoft la reserva para uso del sistema, por lo que es una mejora opcional, no una garantía de compatibilidad. Si falla, conservar el diagnóstico de bloqueo e informar que la identificación quedó incompleta.
- La consulta incluye referencias abiertas a la carpeta, incluso por procesos cuya ubicación de trabajo es esa carpeta. No revela qué apertura concreta deniega compartir borrado. `Process::directoryReference` distingue estos candidatos de los procesos encontrados por Restart Manager sobre archivos.
- La lista resumida de todos los procesos identificados aparece antes de los detalles y no se recorta por el límite de 200 detalles. No prometer identificar todos los procesos que existen: los permisos y el sistema de archivos pueden limitar la consulta.
- Cada proceso requiere autorización individual. Antes de actuar y entre cierres se vuelve a analizar: no cerrar procesos nuevos no autorizados ni aquellos que ya no retienen la selección. Una negativa no se sustituye por un cierre masivo.
- Se comprueban PID y hora de inicio antes de cerrar y antes de finalizar para evitar actuar sobre un PID reutilizado. Se intenta `WM_CLOSE` en las ventanas visibles sin propietario y se esperan hasta cinco segundos cuando se solicitó cierre normal. Si sigue apareciendo como usuario de la selección, puede usarse `TerminateProcess` con la autorización individual explícita.
- La confirmación advierte que la finalización puede perder cambios y cerrar otras ventanas o chats del mismo programa. Esto afecta también a candidatos como Explorador o Codex. No usar el cierre real de esos procesos para verificar el diagnóstico del usuario.
- No cerrar UnlockIt, PID 0/4 ni procesos que `IsProcessCritical` identifique como críticos. Si no se puede comprobar la condición de crítico, no cerrarlo. La solicitud de cierre y la finalización fallidas deben quedar en el resultado; el acceso denegado ofrece elevación.
- Ser administrador no garantiza resolver permisos explícitos, protecciones del sistema o fallos del proveedor de nube. El desbloqueo actual no toma posesión ni modifica listas de permisos.
- Se muestran hasta 200 detalles, sin limitar por ello el recorrido. Los errores incluyen ruta, mensaje y código de Windows. No presentar análisis con errores, cancelaciones u omisiones como una comprobación completa.

## OneDrive y puntos de nube

- No omitir todos los elementos `FILE_ATTRIBUTE_REPARSE_POINT`: OneDrive utiliza puntos de nube y hacerlo excluiría contenido importante.
- `CloudReparse` consulta `FileAttributeTagInfo` sobre el punto mediante `FILE_FLAG_OPEN_REPARSE_POINT`. Reconoce la familia `IO_REPARSE_TAG_CLOUD` usando `IO_REPARSE_TAG_CLOUD_MASK`.
- Se revisan recursivamente los puntos de nube reconocidos. Los demás enlaces y puntos de redirección se informan y se omiten. Antes de modificar se revisan nuevamente atributos y tipo del elemento.
- Un indicador de nube no confirma un bloqueo ni identifica por sí solo a OneDrive: puede ser otro proveedor compatible. Reflejarlo en el texto.
- Para archivos de nube con indicadores `OFFLINE`, `RECALL_ON_OPEN` o `RECALL_ON_DATA_ACCESS`, el análisis actual es parcial: revisa atributos y permisos; omite marca de Internet y programas en uso. No afirmar que comprobó todo el contenido ni prometer que ninguna operación del proveedor descargará metadatos o datos.
- Se identifican varios errores `ERROR_CLOUD_FILE_*`: proveedor no iniciado o terminado, sincronización pendiente, red, autenticación, acceso y archivos en uso, entre otros. Mostrar el error recibido y recomendar revisar el proveedor. No inventar un estado de sincronización si Windows no lo informó.
- Quitar solo lectura no cambia permisos de seguridad, no repara sincronización y no garantiza poder borrar una carpeta.
- Windows puede impedir arrastrar desde un Explorador sin elevar hacia una instancia elevada. El reinicio conserva la selección y el selector permite elegir otros archivos.

## Compilación y pruebas

### Versionado automático

- `BuildVersion.txt` guarda la última versión reservada. Comenzó en `1.0.0` para generar `1.0.1` en la primera compilación. No reiniciarlo ni eliminarlo al limpiar.
- El objetivo MSBuild `GenerateAppVersion` ejecuta `build/Increment-Version.ps1` antes de `PreBuildEvent`, salvo en compilaciones de diseño. Se deshabilita la comprobación rápida de proyecto actualizado para que las compilaciones solicitadas a Visual Studio lleguen a MSBuild.
- El script incrementa el parche y genera `AppVersion.generated.h` dentro de `$(IntDir)`. No editar el encabezado generado. Las inclusiones de C++ y recursos usan esa carpeta.
- El contador es compartido entre configuraciones y plataformas y usa un bloqueo exclusivo de archivo para serializar las reservas. Cada configuración tiene su propio encabezado.
- El título y `AppVersion.rc` usan las mismas macros generadas. El recurso de versión se mantiene separado de `UnlockIt.rc`, que es un archivo UTF-16 de la plantilla. Conservar su codificación si se modifica.
- Verificar `APP_WINDOW_TITLE` en el encabezado y `FileVersion` / `ProductVersion` del ejecutable tras compilar.
- Una compilación fallida puede consumir un número. Limpiar o compilar solo las pruebas no lo incrementa. Los componentes respetan el rango de 16 bits de Windows y el parche desborda hacia la versión menor.
- Verificación del cambio: el script generó `1.0.1` y `1.0.2` usando un contador de prueba independiente; la compilación real Release/x64 produjo `1.0.1`. Se comprobaron `FileVersion`, `ProductVersion` y la cadena de título dentro del ejecutable. Esto no sustituye una inspección visual de la barra de título ni una prueba del botón Compilar de Visual Studio.

### Comandos y comprobaciones

Ejecutar desde la raíz. Esta fue la ubicación de MSBuild en el equipo; si cambia la instalación, localizarla con `vswhere.exe`.

```powershell
$msbuildPath = 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe'
& $msbuildPath UnlockIt.vcxproj /p:Configuration=Debug /p:Platform=x64 /v:minimal /nologo
& $msbuildPath UnlockIt.vcxproj /p:Configuration=Release /p:Platform=x64 /v:minimal /nologo
& $msbuildPath tests\EngineTests.vcxproj /p:Configuration=Debug /p:Platform=x64 /v:minimal /nologo
& .\x64\Tests\EngineTests.exe
```

- Salidas: `x64/Debug/UnlockIt.exe`, `x64/Release/UnlockIt.exe` y `x64/Tests/EngineTests.exe`.
- Comprobar los códigos de salida; la existencia de un ejecutable anterior no prueba una compilación nueva.
- Las pruebas crean datos propios en `x64/test-data-<PID>`, restauran los permisos que alteran y limpian esos datos. No usar documentos reales o carpetas de OneDrive del usuario para pruebas sin autorización específica.
- Cubren recursión, rutas Unicode y espacios, selección duplicada, diagnóstico de rutas internas, solo lectura de archivos y carpetas, marca de Internet, conservación de contenido y otros atributos, rutas inexistentes, cancelación y acceso denegado.
- También prueban permisos de borrado del hijo y alternativa del padre, y bloqueos reales de un archivo o directorio interno mantenidos por un proceso auxiliar.
- Las pruebas de ubicación de trabajo usan dos auxiliares reteniendo la misma carpeta. Comprueban que una identidad antigua no se cierra, que autorizar un proceso conserva al otro, y que autorizar el segundo libera el bloqueo sin borrar el contenido.
- `EngineTests.exe --analyze <ruta>` ejecuta únicamente `Analyze` y escribe un diagnóstico UTF-8; no desbloquea, no altera atributos ni cierra procesos. Puede usarse sobre una ruta real autorizada por el usuario para obtener evidencia sin accionar los cierres de la interfaz.
- Los auxiliares se coordinan mediante eventos para mantener y liberar bloqueos. No cerrar programas reales durante estas pruebas.
- En esta sesión Restart Manager devolvió error 29 (`ERROR_WRITE_FAULT`) al ejecutar pruebas con acceso restringido al registro. El mismo ejecutable pasó con el acceso necesario. Esto no demostró un fallo del desbloqueo de atributos: identificar el origen de los errores antes de cambiar código o pedir elevar la aplicación por ese motivo.

## Estado verificado y pendientes

Última verificación registrada: **15 de noviembre de 2026**, hora de Argentina.

- Compilaciones Debug/x64 y Release/x64 correctas tras agregar carpetas de solo lectura y comprobaciones de eliminación.
- Todos los grupos de pruebas existentes pasaron con las modificaciones finales.
- Caso real autorizado: `H:\matias_chamu2`, disco NTFS, 7 archivos y 4 carpetas contando la raíz. El análisis inicial encontró un bloqueo de compartir borrado en la carpeta principal, sin solo lectura, marcas de Internet, errores de nube ni restricciones de permisos detectadas. La consulta adicional encontró referencias de `explorer.exe` y de procesos de Codex (`codex.exe`, `node_repl.exe`, `node.exe`). Los PID son una instantánea y deben consultarse de nuevo, no fijarse en código.
- No se desbloqueó ni borró esa carpeta ni se cerraron sus procesos reales. Esta evidencia confirma aperturas de carpeta; no demuestra cuál de sus referencias en particular impide el borrado. No atribuir el caso a OneDrive solo por los nombres de archivos internos.
- La versión Release/x64 `1.0.2` incorpora identificación de aperturas de carpetas y autorización por proceso. Los siete grupos de pruebas del motor pasaron, incluida la finalización selectiva de dos auxiliares y la protección de una identidad de proceso antigua. El diagnóstico real se guardó en `diagnostics/matias_chamu2.txt` como instantánea local; no usar sus PID guardados para cerrar procesos posteriormente.
- La compilación Release/x64 `1.0.3` incorpora el candado rompiéndose como icono del ejecutable y la ventana. Se verificaron los diez tamaños de ambos recursos integrados (107 y 108), comparándolos byte a byte con el ICO regenerado, y se inspeccionó la imagen de 256 píxeles extraída. También se verificaron el título generado y presente en el ejecutable, FileVersion y ProductVersion. Este cambio solo afecta recursos y archivos de proyecto: no se repitieron las pruebas del motor ni se inspeccionó la ventana en ejecución.
- La versión Release/x64 `1.0.4` cambia los tamaños menores de 256 a DIB de 32 bits con máscara AND, tras el aviso del usuario de que el candado no aparecía en Detalles. Antes del cambio, ExtractIconExW y SHGetFileInfoW ya devolvían una imagen visible de 16 píxeles; por ello no se confirmó que la compresión fuera la causa. Tras recompilar, PrivateExtractIconsW extrajo correctamente los diez tamaños y la lista pequeña real del Explorador (SHGetFileInfoW con SHGFI_SYSICONINDEX | SHGFI_SMALLICON e ImageList_GetIcon) devolvió el candado visible de 16 x 16 con transparencia. Se notificó SHCNE_UPDATEITEM para el ejecutable, se verificaron título y versiones 1.0.4 y se inspeccionó la imagen pequeña extraída. La comprobación visual de Detalles sigue pendiente: ambas capturas fallaron (FrameArrived timed out / window capture timed out). No se reinició el Explorador ni se borró su caché global; no se repitieron las pruebas del motor para este cambio de recursos.
- Falta verificar un caso real de fallo de sincronización o borrado en OneDrive. Hay reconocimiento de puntos y errores de nube, pero no validación integral con ese servicio.
- La revisión visual quedó pendiente: las capturas fallaron con tiempos de espera (`FrameArrived timed out` / `window capture timed out`). No afirmar que se inspeccionó visualmente la interfaz.
- El diálogo UAC, el arrastre real desde el Explorador y el cierre de una aplicación real con cambios pendientes requieren verificación manual de extremo a extremo. Las pruebas del motor no sustituyen esas comprobaciones.

- La versión Release/x64 `1.0.5` cambia el nombre de la aplicación a **UnlockIt**, incluidos ventana, mensajes, recursos, metadatos del ejecutable, solución, proyecto, fuentes e iconos. Se compiló `UnlockIt.vcxproj` correctamente y se verificaron el título generado y presente en `x64/Release/UnlockIt.exe`, FileDescription, InternalName, OriginalFilename, ProductName y ambas versiones. Se conservó UTF-16 en `UnlockIt.rc`. Este cambio solo modifica nombres: no se repitieron las pruebas del motor ni se inspeccionó visualmente la ventana.

## Referencias técnicas usadas

- [Atributos: solo lectura y nube](https://learn.microsoft.com/en-us/windows/win32/fileio/file-attribute-constants).
- [DeleteFileW: permisos del elemento y del padre](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-deletefilew).
- [Seguridad y derechos de acceso](https://learn.microsoft.com/en-us/windows/win32/fileio/file-security-and-access-rights).
- [Etiquetas de puntos de redirección](https://learn.microsoft.com/en-us/windows/win32/fileio/reparse-point-tags).
- [Restart Manager: recursos y errores del registro](https://learn.microsoft.com/en-us/windows/win32/api/restartmanager/nf-restartmanager-rmregisterresources).
- [Unblock-File y Zone.Identifier](https://learn.microsoft.com/en-us/powershell/module/microsoft.powershell.utility/unblock-file).
- [FILE_INFORMATION_CLASS: clase 47 reservada para uso del sistema](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/ne-wdm-_file_information_class).
- [Comprobación de procesos críticos](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-isprocesscritical).
- [TerminateProcess y sus riesgos para datos pendientes](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-terminateprocess).
