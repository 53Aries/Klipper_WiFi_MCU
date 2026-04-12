@echo off
set IDF_PATH=C:\esp\v6.0\esp-idf
set IDF_TOOLS_PATH=C:\esp\v6.0
C:\Python313\python.exe C:\esp\v6.0\esp-idf\tools\idf_tools.py check > C:\KlipperESPwifi\idf_check.txt 2>&1
echo exit code: %errorlevel% >> C:\KlipperESPwifi\idf_check.txt
