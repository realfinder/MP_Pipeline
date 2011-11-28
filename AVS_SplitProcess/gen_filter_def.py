
def generate_output():
    p = FilterParam
    write_definition("MP_Pipeline",
        p("s", "script", optional=False),
    )

PARAM_TYPES_FULL = {
    "c": ("PClip", "AsClip"),
    "b": ("bool", "AsBool"),
    "i": ("int", "AsInt"),
    "f": ("float", "AsFloat"),
    "s": ("const char*", "AsString"),
}

PARAM_TYPES = { k: v[0] for k, v in PARAM_TYPES_FULL.items() }

AVSVALUE_FUNCTIONS = { k: v[1] for k, v in PARAM_TYPES_FULL.items() }

class FilterParam:
    def __init__(
            self, 
            type, 
            name, 
            field_name=None,
            c_type=None, 
            optional=True,
            has_field=True,
            default_value=None):
        if type not in PARAM_TYPES.keys():
            raise ValueError("Type {} is not supported.".format(type))

        self.type = type
        self.name = name
        self.field_name = field_name or name
        self.c_type = c_type or PARAM_TYPES[type]
        self.custom_c_type = c_type is not None
        self.optional = optional
        self.has_field = has_field
        self.default_value = default_value

OUTPUT_TEMPLATE = """
#pragma once

/*************************
 * Script generated code *
 *     Do not modify     *
 *************************/

#include <stddef.h>
#include "avisynth.h"

static const char* {filter_name_u}_AVS_PARAMS = "{avs_params}";

typedef struct _{filter_name_u}_RAW_ARGS
{{
    AVSValue {init_param_list};
}} {filter_name_u}_RAW_ARGS;

#define {filter_name_u}_ARG_INDEX(name) (offsetof({filter_name_u}_RAW_ARGS, name) / sizeof(AVSValue))

#define {filter_name_u}_ARG(name) args[{filter_name_u}_ARG_INDEX(name)]

class {filter_name}_parameter_storage_t
{{
protected:

    {class_field_def}

public:

    {filter_name}_parameter_storage_t(const {filter_name}_parameter_storage_t& o)
    {{
        {class_field_copy}
    }}

    {filter_name}_parameter_storage_t( {init_param_list_with_field_func_def} )
    {{
        {class_field_init}
    }}

    {filter_name}_parameter_storage_t( AVSValue args )
    {{
        {class_field_init_avsvalue}
    }}
}};

#define {filter_name_u}_CREATE_CLASS(klass) new klass( {init_param_list_without_field_invoke}{filter_name}_parameter_storage_t( {init_param_list_with_field_invoke} ) )

#ifdef {filter_name_u}_SIMPLE_MACRO_NAME

#ifdef SIMPLE_MACRO_NAME
#error Simple macro name has already been defined for SIMPLE_MACRO_NAME
#endif

#define SIMPLE_MACRO_NAME {filter_name}

#define ARG {filter_name_u}_ARG

#define CREATE_CLASS {filter_name_u}_CREATE_CLASS

#endif

#ifdef {filter_name_u}_IMPLEMENTATION

AVSValue __cdecl Create_{filter_name}(AVSValue args, void* user_data, IScriptEnvironment* env);

void Register_{filter_name}(IScriptEnvironment* env, void* user_data=NULL)
{{
    env->AddFunction("{filter_name}", 
        {filter_name_u}_AVS_PARAMS,
        Create_{filter_name},
        user_data);
}}

#else

void Register_{filter_name}(IScriptEnvironment* env, void* user_data=NULL);

#endif

"""

def build_avs_params(params):
    def get_param(param):
        return param.optional and '[{0.name}]{0.type}'.format(param) or param.type

    return ''.join([get_param(x) for x in params])

def build_init_param_list_invoke(
    params, 
    predicate=lambda x:True, 
    add_trailing_comma=False):
    
    lst = [x.custom_c_type and "({}){}".format(x.c_type, x.field_name) or x.field_name for x in params if predicate(x)]
    if add_trailing_comma:
        return ''.join([x + ", " for x in lst])
    else:
        return ", ".join(lst)

def build_declaration_list(params, name_prefix='', predicate=lambda x:True):
    return ["{} {}{}".format(x.c_type, name_prefix, x.field_name) for x in params if predicate(x)]

def build_init_param_list_func_def(params, predicate=lambda x: True):
    return ", ".join(build_declaration_list(params, predicate=predicate))

def build_class_field_def(params):
    return "\n    ".join([x + "; " for x in build_declaration_list(params, "_", lambda x: x.has_field)])

def build_class_field_init(params):
    return "\n        ".join(["_{0} = {0}; ".format(x.field_name) for x in params if x.has_field])

def build_class_field_init_avsvalue_item(filter_name, param):
    default = ""
    if param.default_value is not None:
        if isinstance(param.default_value, str):
            default = "{}".format(param.default_value)
        else:
            default = param.default_value

    ret = "_{param_name} = {c_type}{filter_name_u}_ARG({param_name}).{as_val}({default});".format(
        param_name=param.field_name,
        c_type=param.custom_c_type and "({})".format(param.c_type) or "",
        filter_name_u=filter_name.upper(),
        as_val=AVSVALUE_FUNCTIONS[param.type],
        default=default,
    )
    return ret
    

def build_class_field_init_avsvalue(filter_name, params):
    return "\n        ".join([build_class_field_init_avsvalue_item(filter_name, x) for x in params if x.has_field])

def build_class_field_copy(params):
    return "\n        ".join(["_{0} = o._{0}; ".format(x.field_name) for x in params if x.has_field])

def generate_definition(filter_name, *params):
   format_params = {
       "filter_name": filter_name,
       "filter_name_u": filter_name.upper(),
       "avs_params": build_avs_params(params),
       "init_param_list": ', '.join([x.field_name for x in params]),
       "init_param_list_with_field_invoke": build_init_param_list_invoke(params, lambda x: x.has_field),
       "init_param_list_without_field_invoke": build_init_param_list_invoke(params, lambda x: not x.has_field, True),
       "init_param_list_with_field_func_def": build_init_param_list_func_def(params, lambda x: x.has_field),
       "class_field_def": build_class_field_def(params),
       "class_field_init": build_class_field_init(params),
       "class_field_init_avsvalue": build_class_field_init_avsvalue(filter_name, params),
       "class_field_copy": build_class_field_copy(params),
   }

   return OUTPUT_TEMPLATE.format(**format_params)

def write_definition(filter_name, *params):
    with open("{0}.def.h".format(filter_name), "w") as f:
        f.write(generate_definition(filter_name, *params))

if __name__ == "__main__":
    generate_output()

