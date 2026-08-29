@rem This script builds MaterialX JavaScript on Windows. The final command starts a local server, allowing you to
@rem run the MaterialX Web Viewer locally by entering 'http://localhost:8080' in the search bar of your browser.
@rem If PathTracerGlslShaderGenerator ABI changed, rerun this script and republish generated artifacts.
@echo --------------------- Setup Emscripten ---------------------
@echo on
@rem Edit the following paths to match your local locations for the Emscripten and MaterialX projects.
set EMSDK_LOCATION=d:\emsdk
set MATERIALX_LOCATION=D:\WebGL2\MaterialX\MaterialX-rva
call %EMSDK_LOCATION%/emsdk.bat install 4.0.8
call %EMSDK_LOCATION%/emsdk.bat activate 4.0.8
if NOT ["%errorlevel%"]==["0"] pause
@echo --------------------- Build MaterialX With JavaScript ---------------------
@echo on
cd %MATERIALX_LOCATION%
@rem cmake -S . -B javascript/build -DMATERIALX_BUILD_JS=ON -DMATERIALX_EMSDK_PATH=%EMSDK_LOCATION% -G Ninja
@rem cmake --build javascript/build --target install --config RelWithDebInfo --parallel 2
cmake -S . -B javascript/build -DMATERIALX_BUILD_JS=ON -DMATERIALX_EMSDK_PATH=%EMSDK_LOCATION% -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build javascript/build --target install
if NOT ["%errorlevel%"]==["0"] pause
@echo --------------------- Install JavaScript Dependencies ---------------------
@echo on
cd javascript
call npm install
if NOT ["%errorlevel%"]==["0"] pause
@rem @echo --------------------- Run JavaScript Tests ---------------------
@rem @echo on
@rem cd MaterialXTest
@rem call npx playwright install chromium
@rem call npm run test
@rem call npm run test:browser
@rem if NOT ["%errorlevel%"]==["0"] pause
@rem @echo --------------------- Run Interactive Viewer ---------------------
@rem @echo on
@rem cd ..\MaterialXView
@rem call npm run build
@rem call npm run start
@rem if NOT ["%errorlevel%"]==["0"] pause
