@echo off
cd /d "D:\PROJETOS\Archeage Emulator\Emulator\tools\umap-dump"
dotnet build -c Release -o "D:\PROJETOS\Archeage Emulator\Emulator\out\agent-team-reports\quest\umap-extract\bin" > "D:\PROJETOS\Archeage Emulator\Emulator\out\agent-team-reports\quest\umap-extract\build.log" 2>&1
echo exit=%errorlevel%
