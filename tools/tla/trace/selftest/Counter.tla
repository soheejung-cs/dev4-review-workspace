---- MODULE Counter ----
\* selftest 의 모델 — Inc 하나가 원자적으로 c 를 1 올린다(잠금 판의 계약).
EXTENDS Naturals
VARIABLES c
Init == c = 0
Inc == c' = c + 1
====
