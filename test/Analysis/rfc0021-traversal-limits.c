// RUN: not %weavec --checked-function=variable_limit -Wno-error=weavec %s -- 2>&1 | FileCheck %s
// RFC 0021: exceeding the fixed traversal-variable budget cannot hide
// unresolved coverage, including after diagnostic demotion.
// CHECK: warning: cannot establish checked safety: traversal variable limit reached [weavec::checking-incomplete]
// CHECK: error: checked safety requirements were not established

void variable_limit(
    char *p0, char *p1, char *p2, char *p3, char *p4,
    char *p5, char *p6, char *p7, char *p8, char *p9,
    char *p10, char *p11, char *p12, char *p13, char *p14,
    char *p15, char *p16, char *p17, char *p18, char *p19,
    char *p20, char *p21, char *p22, char *p23, char *p24,
    char *p25, char *p26, char *p27, char *p28, char *p29,
    char *p30, char *p31, char *p32, char *p33, char *p34,
    char *p35, char *p36, char *p37, char *p38, char *p39,
    char *p40, char *p41, char *p42, char *p43, char *p44,
    char *p45, char *p46, char *p47, char *p48, char *p49,
    char *p50, char *p51, char *p52, char *p53, char *p54,
    char *p55, char *p56, char *p57, char *p58, char *p59,
    char *p60, char *p61, char *p62, char *p63, char *p64) {
  while (
      *p0 || *p1 || *p2 || *p3 || *p4 ||
      *p5 || *p6 || *p7 || *p8 || *p9 ||
      *p10 || *p11 || *p12 || *p13 || *p14 ||
      *p15 || *p16 || *p17 || *p18 || *p19 ||
      *p20 || *p21 || *p22 || *p23 || *p24 ||
      *p25 || *p26 || *p27 || *p28 || *p29 ||
      *p30 || *p31 || *p32 || *p33 || *p34 ||
      *p35 || *p36 || *p37 || *p38 || *p39 ||
      *p40 || *p41 || *p42 || *p43 || *p44 ||
      *p45 || *p46 || *p47 || *p48 || *p49 ||
      *p50 || *p51 || *p52 || *p53 || *p54 ||
      *p55 || *p56 || *p57 || *p58 || *p59 ||
      *p60 || *p61 || *p62 || *p63 || *p64) {
    break;
  }
}
