#include <mruby.h>
#include <mruby/array.h>
#include <mruby/string.h>
#include <mruby/variable.h>
#include <mruby/compile.h>
#include <mruby/irep.h>
#include <mruby/presym.h>
#include <mruby/error.h>
#include <string.h>

extern const uint8_t hypha_main[];

int main(const int argc, const char* argv[]) {
    mrb_state* mrb = NULL;
    int exit_code = 0;
    mrb = mrb_open();
    if (!mrb) {
        return 1;
    }
    mrb_value ARGV = mrb_ary_new_capa(mrb, argc);
    for (int i = 0; i < argc; i++) {
        mrb_value arg = mrb_str_new_static_frozen(mrb, argv[i], strlen(argv[i]));
        mrb_ary_push(mrb, ARGV, arg);
    }
    mrb_obj_freeze(mrb, ARGV);
    mrb_define_const_id(mrb, mrb->object_class, MRB_SYM(ARGV), ARGV);
    mrb_value args = mrb_load_string(mrb, "TypedArgs.opts");
    if (mrb->exc) {
        mrb_print_error(mrb);
        exit_code = 1;
        goto done;
    }
    mrb_load_irep(mrb, hypha_main);  /* runs top level, defines main */
    if (mrb->exc) { mrb_print_error(mrb); exit_code = 1; goto done; }
	mrb_funcall_argv(mrb, mrb_top_self(mrb), MRB_SYM(main), 1, &args);
    if (mrb->exc) {
        mrb_print_error(mrb);
        exit_code = 1;
    }
done:
    mrb_close(mrb);
    return exit_code;
}