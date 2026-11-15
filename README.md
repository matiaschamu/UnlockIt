# AppUnlock

Aplicación para Windows que analiza archivos y carpetas para ayudar a identificar qué impide modificarlos o borrarlos. Tiene una ventana simple, sin menús y siempre por encima de las ventanas normales.

## Cómo usarla

1. Abrí `x64/Release/AppUnlock.exe` después de compilar el proyecto.
2. Arrastrá un archivo, una carpeta o varios elementos a **Arrastre aquí**. También podés hacer clic en esa zona para elegir archivos; las carpetas se seleccionan arrastrándolas.
3. Esperá el diagnóstico. Las carpetas se recorren de forma recursiva, incluidas sus subcarpetas. El análisis no borra elementos ni cambia sus atributos o permisos.
4. Revisá las rutas y los problemas encontrados. Si hay acciones disponibles, pulsá **Desbloquear** y confirmá los cambios propuestos.
5. Si un programa pide guardar cambios, resolvé ese aviso. Usá **Volver a analizar** para comprobar qué problemas siguen presentes.

Durante el análisis y el desbloqueo los botones de nuevas operaciones quedan deshabilitados. Si cerrás la ventana durante una operación, la aplicación solicita cancelarla y espera a que termine.

## Qué muestra el diagnóstico

| Comprobación | Qué significa |
| --- | --- |
| Marca de Internet | El archivo tiene una marca de procedencia que Windows puede usar para mostrar advertencias. |
| Archivos de solo lectura | Tienen un atributo que puede impedir su modificación o eliminación. |
| Carpetas con marca de solo lectura | Se detectan y se muestra su ruta. Esa marca por sí sola no impide borrar una carpeta en Windows; los permisos y otros bloqueos se revisan por separado. |
| Permisos de borrado | Se comprueba si se puede obtener acceso para eliminar el elemento o hacerlo desde su carpeta contenedora. Una restricción en un elemento interno puede impedir borrar la carpeta principal. La comprobación no intenta borrarlo. |
| Programas en uso | Muestra los nombres y PID de los programas que Windows puede identificar, junto con las rutas de los archivos usados. |
| Procesos con carpetas abiertas | Cuando se detecta un bloqueo de borrado en una carpeta, intenta listar los procesos que la mantienen abierta, incluso si no tienen archivos internos abiertos. Son candidatos: la consulta no indica cuál de sus aperturas impide borrar. |
| Bloqueos de acceso | Informa cuando un programa impide acceder a un archivo o eliminar un elemento, aunque Windows no permita identificar al responsable. |
| Nube / OneDrive | Identifica puntos de nube compatibles e informa errores del proveedor que Windows devuelve durante las comprobaciones. El indicador de nube por sí solo no significa que exista un bloqueo. |
| Errores y elementos omitidos | Muestra rutas inaccesibles o inexistentes, errores de Windows y enlaces que se omitieron. |

Las comprobaciones alcanzan los elementos internos de la selección. Se muestran hasta 200 detalles; ese límite es de presentación y no limita el recorrido. Si se cancela la operación o hay errores, el análisis puede quedar incompleto.

La lista resumida de procesos identificados se muestra antes de los detalles y no queda recortada por el límite de 200 detalles. Puede incluir el Explorador, editores, terminales o procesos de Codex con esa carpeta como ubicación de trabajo. Windows no garantiza que se puedan identificar todos los procesos en todos los casos.

## Qué hace Desbloquear

El botón se habilita cuando hay marcas de Internet, archivos o carpetas de solo lectura, o procesos identificados. Los cambios de atributos requieren confirmación y se pregunta por cada proceso por separado.

- Quita la marca `Zone.Identifier` de los archivos detectados.
- Quita el atributo de solo lectura de los archivos y carpetas detectados, de forma recursiva.
- Por cada proceso muestra su nombre y PID, indicando si se detectó por archivos en uso o como candidato de una carpeta abierta. **Sí** autoriza cerrar ese proceso; **No** lo deja abierto; **Cancelar** cancela la operación antes de iniciar los cambios o cierres.
- Solo actúa sobre los procesos autorizados individualmente. Intenta cerrar sus ventanas normalmente y, si el proceso continúa reteniendo la selección, puede finalizarlo. La confirmación advierte que se pueden perder cambios sin guardar y cerrar otras ventanas o chats del mismo programa.
- Revisa de nuevo la selección entre cierres. No cierra procesos nuevos que no se autorizaron y omite a los que ya no figuran reteniendo la selección.
- Repite el análisis y muestra cuántos elementos se actualizaron, los errores y lo que sigue pendiente.

Los cambios de atributos conservan el contenido y los otros atributos de los archivos. **Desbloquear no borra la selección**, no toma posesión ni cambia permisos de seguridad. Finalizar un proceso sí puede perder datos sin guardar; guardá tu trabajo antes de autorizarlo. Se excluyen AppUnlock y los procesos críticos de Windows; algunos procesos protegidos no podrán cerrarse. Si el único problema detectado no tiene una acción disponible, puede que el botón no se habilite.

Solo desbloqueá archivos cuyo origen conozcas: quitar la marca de Internet elimina una advertencia asociada a su procedencia, pero no comprueba que sean seguros.

## Permisos de administrador

Cuando se detecta falta de permisos, la aplicación pregunta si querés reiniciarla como administrador. Si aceptás, Windows solicita autorización y la nueva instancia vuelve a analizar la misma selección. El desbloqueo requiere pulsar el botón nuevamente.

Si rechazás o cancelás la autorización, podés seguir revisando el diagnóstico y usar el botón **Reiniciar como administrador** más adelante.

Ejecutar como administrador no garantiza resolver restricciones de seguridad o problemas de sincronización. Además, Windows puede impedir arrastrar desde un Explorador sin elevar hacia la aplicación elevada: la selección original se conserva y podés usar el selector para elegir otros archivos.

## Carpetas de OneDrive

La aplicación reconoce los puntos de nube compatibles para poder revisar sus carpetas recursivamente, en lugar de omitirlos como si todos fueran enlaces a otra ubicación.

Si Windows informa problemas como proveedor no iniciado, sincronización pendiente, falta de conexión, acceso denegado o archivo en uso, se muestran la ruta y el error. Puede ser necesario abrir OneDrive o el proveedor correspondiente y resolver allí el estado de sincronización o los permisos.

En archivos de nube que no están completamente disponibles en este equipo, el análisis es parcial: revisa atributos y permisos, pero omite la marca de Internet y la consulta de programas en uso. Quitar la marca de solo lectura no repara la sincronización ni garantiza que la carpeta pueda borrarse.

## Límites conocidos

- Windows no siempre permite identificar todos los programas responsables de un bloqueo.
- La consulta adicional de procesos de carpetas depende de una función nativa opcional de Windows. Si el sistema no la admite, se informa que la carpeta está bloqueada y que no se pudo completar esa identificación.
- Las carpetas abiertas, servicios, procesos sin ventanas y bloqueos del sistema pueden requerir intervención manual.
- Se omiten los enlaces y puntos de redirección distintos de los puntos de nube reconocidos.
- La aplicación comprueba accesos y condiciones actuales; no garantiza que una eliminación posterior vaya a completarse.
- Se probaron bloqueos y restricciones en carpetas locales. Queda pendiente verificar un caso real de fallo de OneDrive y revisar visualmente la interfaz, ya que la captura de ventana falló durante el desarrollo.
- El arrastre desde el Explorador, el diálogo UAC y el cierre de aplicaciones reales con cambios pendientes aún requieren verificación manual de extremo a extremo.

## Compilación

El icono del ejecutable y de la ventana es el candado rompiéndose de `AppUnlock-icon.png`, con fondo transparente. `AppUnlock.ico` y `small.ico` contienen versiones de 16 a 256 píxeles. Los tamaños menores de 256 usan el formato clásico DIB de 32 bits con máscara de transparencia para las vistas pequeñas del Explorador; solo el de 256 se comprime como PNG. Si se cambia el PNG, ejecutá `powershell -NoProfile -ExecutionPolicy Bypass -File build\Convert-Icon.ps1` antes de compilar para regenerar ambos iconos.

Si una ventana del Explorador conserva un icono anterior después de recompilar, pulsá **F5** en esa carpeta para actualizar la vista.

La barra de título muestra el nombre y la versión, por ejemplo **AppUnlock - ver 1.0.1**. Cada compilación de la aplicación incrementa automáticamente el último número: `1.0.1`, `1.0.2`, `1.0.3`… La misma versión queda en las propiedades del ejecutable.

El contador se guarda en `BuildVersion.txt` y se comparte entre configuraciones y plataformas. No borrarlo ni reiniciarlo al limpiar los archivos de compilación. El número se reserva al comenzar a compilar; un intento fallido puede consumir una versión. Limpiar el proyecto, abrirlo o compilar únicamente las pruebas no incrementa el contador.

Requiere Visual Studio con las herramientas de escritorio para C++, toolset `v145` y Windows SDK 10.0. El proyecto usa C++20, Unicode y archivos fuente UTF-8.

Abrí `AppUnlock.slnx`, seleccioná **Release / x64** y compilá. El ejecutable queda en `x64/Release/AppUnlock.exe`. También se verificó la compilación **Debug / x64**.

Desde PowerShell en la raíz del proyecto, con la instalación usada durante el desarrollo:

```powershell
$msbuildPath = 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe'
& $msbuildPath AppUnlock.vcxproj /p:Configuration=Release /p:Platform=x64 /v:minimal /nologo
```

Si Visual Studio está en otra ubicación, ajustá la ruta de MSBuild.

## Pruebas

El proyecto de pruebas se compila por separado; no está incluido en la solución principal. Ejecutá desde la raíz del proyecto:

```powershell
$msbuildPath = 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe'
& $msbuildPath tests\EngineTests.vcxproj /p:Configuration=Debug /p:Platform=x64 /v:minimal /nologo
& .\x64\Tests\EngineTests.exe
```

Las pruebas crean y eliminan únicamente datos temporales propios en `x64/test-data-<PID>`. Cubren recursión, nombres Unicode y espacios, selección duplicada, diagnósticos con rutas internas, solo lectura en archivos y carpetas, marcas de Internet, conservación de contenido y otros atributos, rutas inexistentes, cancelación, restricciones de permisos y bloqueos reales mantenidos por procesos auxiliares de prueba.

También cubren procesos que retienen una carpeta como ubicación de trabajo, su identificación, la conservación de los no autorizados y el cierre de los autorizados. Las pruebas de cierre usan exclusivamente sus procesos auxiliares.

Para ejecutar el mismo motor en modo de diagnóstico de solo lectura, sin abrir la interfaz ni desbloquear nada:

```powershell
& .\x64\Tests\EngineTests.exe --analyze 'H:\matias_chamu2'
```

Las compilaciones Debug/x64 y Release/x64 y los grupos de pruebas existentes finalizaron correctamente el 14 de noviembre de 2026. La consulta de programas requiere acceso al registro del usuario; si Restart Manager devuelve error 29 en un entorno restringido, revisar ese acceso antes de atribuirlo a un fallo de los archivos analizados.

Las decisiones técnicas, referencias y pendientes para continuar el desarrollo están en [AGENTS.md](AGENTS.md).
