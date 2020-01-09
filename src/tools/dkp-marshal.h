
#ifndef __devkit_power_marshal_MARSHAL_H__
#define __devkit_power_marshal_MARSHAL_H__

#include	<glib-object.h>

G_BEGIN_DECLS

/* VOID:BOOLEAN,STRING,BOOLEAN,INT,INT,STRING,DOUBLE (../src/dkp-marshal.list:1) */
extern void devkit_power_marshal_VOID__BOOLEAN_STRING_BOOLEAN_INT_INT_STRING_DOUBLE (GClosure     *closure,
                                                                                     GValue       *return_value,
                                                                                     guint         n_param_values,
                                                                                     const GValue *param_values,
                                                                                     gpointer      invocation_hint,
                                                                                     gpointer      marshal_data);

/* VOID:STRING,BOOLEAN,STRING,BOOLEAN,INT,INT,STRING,DOUBLE (../src/dkp-marshal.list:2) */
extern void devkit_power_marshal_VOID__STRING_BOOLEAN_STRING_BOOLEAN_INT_INT_STRING_DOUBLE (GClosure     *closure,
                                                                                            GValue       *return_value,
                                                                                            guint         n_param_values,
                                                                                            const GValue *param_values,
                                                                                            gpointer      invocation_hint,
                                                                                            gpointer      marshal_data);

/* VOID:STRING,INT (../src/dkp-marshal.list:3) */
extern void devkit_power_marshal_VOID__STRING_INT (GClosure     *closure,
                                                   GValue       *return_value,
                                                   guint         n_param_values,
                                                   const GValue *param_values,
                                                   gpointer      invocation_hint,
                                                   gpointer      marshal_data);

/* VOID:POINTER,BOOLEAN (../src/dkp-marshal.list:4) */
extern void devkit_power_marshal_VOID__POINTER_BOOLEAN (GClosure     *closure,
                                                        GValue       *return_value,
                                                        guint         n_param_values,
                                                        const GValue *param_values,
                                                        gpointer      invocation_hint,
                                                        gpointer      marshal_data);

/* VOID:POINTER,POINTER (../src/dkp-marshal.list:5) */
extern void devkit_power_marshal_VOID__POINTER_POINTER (GClosure     *closure,
                                                        GValue       *return_value,
                                                        guint         n_param_values,
                                                        const GValue *param_values,
                                                        gpointer      invocation_hint,
                                                        gpointer      marshal_data);

/* VOID:POINTER,POINTER,BOOLEAN (../src/dkp-marshal.list:6) */
extern void devkit_power_marshal_VOID__POINTER_POINTER_BOOLEAN (GClosure     *closure,
                                                                GValue       *return_value,
                                                                guint         n_param_values,
                                                                const GValue *param_values,
                                                                gpointer      invocation_hint,
                                                                gpointer      marshal_data);

G_END_DECLS

#endif /* __devkit_power_marshal_MARSHAL_H__ */

