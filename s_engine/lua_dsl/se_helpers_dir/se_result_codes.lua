--============================================================================
-- se_result_codes.lua
-- Application, Function, and Pipeline result code emitters
--============================================================================

-- APPLICATION RESULT CODES (0-5)


register_builtin("SE_RETURN_CONTINUE")
register_builtin("SE_RETURN_HALT")
register_builtin("SE_RETURN_TERMINATE")
register_builtin("SE_RETURN_RESET")
register_builtin("SE_RETURN_DISABLE")
register_builtin("SE_RETURN_SKIP_CONTINUE")

register_builtin("SE_RETURN_FUNCTION_CONTINUE")
register_builtin("SE_RETURN_FUNCTION_HALT")
register_builtin("SE_RETURN_FUNCTION_TERMINATE")
register_builtin("SE_RETURN_FUNCTION_RESET")
register_builtin("SE_RETURN_FUNCTION_DISABLE")
register_builtin("SE_RETURN_FUNCTION_SKIP_CONTINUE")

register_builtin("SE_RETURN_PIPELINE_CONTINUE")
register_builtin("SE_RETURN_PIPELINE_HALT")
register_builtin("SE_RETURN_PIPELINE_TERMINATE")
register_builtin("SE_RETURN_PIPELINE_RESET")
register_builtin("SE_RETURN_PIPELINE_DISABLE")
register_builtin("SE_RETURN_PIPELINE_SKIP_CONTINUE")


function se_return_continue()
    local c = m_call("SE_RETURN_CONTINUE")
    end_call(c)
end

function se_return_halt()
    local c = m_call("SE_RETURN_HALT")
    end_call(c)
end

function se_return_terminate()
    local c = m_call("SE_RETURN_TERMINATE")
    end_call(c)
end

function se_return_reset()
    local c = m_call("SE_RETURN_RESET")
    end_call(c)
end

function se_return_disable()
    local c = m_call("SE_RETURN_DISABLE")
    end_call(c)
end

function se_return_skip_continue()
    local c = m_call("SE_RETURN_SKIP_CONTINUE")
    end_call(c)
end

-- FUNCTION RESULT CODES (6-11)
function se_return_function_continue()
    local c = m_call("SE_RETURN_FUNCTION_CONTINUE")
    end_call(c)
end

function se_return_function_halt()
    local c = m_call("SE_RETURN_FUNCTION_HALT")
    end_call(c)
end

function se_return_function_terminate()
    local c = m_call("SE_RETURN_FUNCTION_TERMINATE")
    end_call(c)
end

function se_return_function_reset()
    local c = m_call("SE_RETURN_FUNCTION_RESET")
    end_call(c)
end

function se_return_function_disable()
    local c = m_call("SE_RETURN_FUNCTION_DISABLE")
    end_call(c)
end

function se_return_function_skip_continue()
    local c = m_call("SE_RETURN_FUNCTION_SKIP_CONTINUE")
    end_call(c)
end

-- PIPELINE RESULT CODES (12-17)
function se_return_pipeline_continue()
    local c = m_call("SE_RETURN_PIPELINE_CONTINUE")
    end_call(c)
end

function se_return_pipeline_halt()
    local c = m_call("SE_RETURN_PIPELINE_HALT")
    end_call(c)
end

function se_return_pipeline_terminate()
    local c = m_call("SE_RETURN_PIPELINE_TERMINATE")
    end_call(c)
end

function se_return_pipeline_reset()
    local c = m_call("SE_RETURN_PIPELINE_RESET")
    end_call(c)
end

function se_return_pipeline_disable()
    local c = m_call("SE_RETURN_PIPELINE_DISABLE")
    end_call(c)
end

function se_return_pipeline_skip_continue()
    local c = m_call("SE_RETURN_PIPELINE_SKIP_CONTINUE")
    end_call(c)
end