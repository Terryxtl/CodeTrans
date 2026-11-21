# CodeTrans R8 rules.
#
# No app-specific rules are needed; JNI is covered elsewhere:
# - NativeCore: JNI binds by Java_com_codetrans_NativeCore_<name>, so only the class name and
#   native method names must survive. The AGP default (proguard-android-optimize.txt)
#   -keepclasseswithmembernames,includedescriptorclasses class * { native <methods>; } keeps them.
#   Its const vals are inlined by kotlinc and never read from C.
# - NativeResult: looked up by name in JNI_OnLoad and constructed from C; kept by its
#   @androidx.annotation.Keep annotation.
