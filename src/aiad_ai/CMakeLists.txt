# aiad_ai - agent engine: Claude Code CLI provider, event reducer, saved contexts, account reads.
# Knows nothing about geometry (the model is reached through MCP -> bridge -> Command Bus).
add_library(aiad_ai STATIC ClaudeProcess.cpp AgentSession.cpp Transcripts.cpp Account.cpp)
target_include_directories(aiad_ai PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(aiad_ai PRIVATE /Zi)
target_link_libraries(aiad_ai PUBLIC aiad_core PRIVATE winhttp)

add_executable(agent_session_test tests/agent_session_test.cpp)
target_link_libraries(agent_session_test PRIVATE aiad_ai)
add_test(NAME agent_session_test COMMAND agent_session_test)

# Live handshake with the installed CLI (no prompt is sent: no tokens). Skips when claude is absent.
add_executable(claude_cli_test tests/claude_cli_test.cpp)
target_link_libraries(claude_cli_test PRIVATE aiad_ai)
add_test(NAME claude_cli_test COMMAND claude_cli_test)
