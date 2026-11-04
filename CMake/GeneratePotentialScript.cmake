file(READ "${TrainerSourceRoot}/Source/BlazblueTrainer/Scripts/GrantAllPotentials.js" PotentialScriptSource)
string(REPLACE "\r\n" "\n" PotentialScriptSource "${PotentialScriptSource}")
string(REPLACE "\n" "\r\n" PotentialScriptSource "${PotentialScriptSource}")
configure_file("${TrainerSourceRoot}/CMake/PotentialScript.h.in" "${TrainerScriptHeader}" @ONLY NEWLINE_STYLE CRLF)
