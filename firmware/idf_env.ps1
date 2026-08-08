# Source this script before idf.py commands when the Espressif installer

# export.ps1 points at a missing Python env (e.g. py3.14 vs py3.11).

$env:IDF_PATH = "C:\Espressif\frameworks\esp-idf-v5.3.1"

$env:IDF_PYTHON_ENV_PATH = "C:\Espressif\python_env\idf5.3_py3.11_env"



$idfTools = @(

    "$env:IDF_PYTHON_ENV_PATH\Scripts",

    "C:\Espressif\tools\xtensa-esp-elf\esp-13.2.0_20240530\xtensa-esp-elf\bin",

    "C:\Espressif\tools\riscv32-esp-elf\esp-13.2.0_20240530\riscv32-esp-elf\bin",

    "C:\Espressif\tools\cmake\3.24.0\bin",

    "C:\Espressif\tools\ninja\1.11.1",

    (Join-Path $env:IDF_PATH "tools")

)

foreach ($toolPath in $idfTools) {

    if ($env:PATH -notlike "*$toolPath*") {

        $env:PATH = "$toolPath;$env:PATH"

    }

}



# Use this instead of "$env:IDF_PATH\tools\idf.py" — PowerShell treats \t as TAB.

$script:IdfPy = Join-Path $env:IDF_PATH "tools/idf.py"



Write-Host "IDF_PATH=$env:IDF_PATH"

Write-Host "IDF_PYTHON_ENV_PATH=$env:IDF_PYTHON_ENV_PATH"

Write-Host "IdfPy=$script:IdfPy"



function idf {

    param([Parameter(ValueFromRemainingArguments = $true)][string[]]$Args)

    & python $script:IdfPy @Args

}


