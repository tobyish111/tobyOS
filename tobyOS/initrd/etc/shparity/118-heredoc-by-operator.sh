s01() {
  if false; then
    cat <<A
S01 skipped
A
  fi
  cat <<B
S01 wanted
B
}
s01
s02() {
  if false; then
    cat <<A
S02 then
A
  elif false; then
    cat <<B
S02 elif
B
  else
    cat <<C
S02 else
C
  fi
  cat <<D
S02 after
D
}
s02
s03() {
  for i in; do
    cat <<A
S03 never
A
  done
  cat <<B
S03 after
B
}
s03
s04() {
  for i in 1 2; do
    cat <<A
S04 first $i
A
    break
    cat <<B
S04 unreached
B
  done
  cat <<C
S04 after
C
}
s04
s05() {
  for i in 1 2 3; do
    cat <<A
S05 pre $i
A
    [ $i = 2 ] && continue
    cat <<B
S05 post $i
B
  done
  cat <<C
S05 after
C
}
s05
s06() {
  case x in
    y) cat <<A
S06 y-arm
A
      ;;
    x) cat <<B
S06 x-arm
B
      ;;
  esac
  cat <<C
S06 after
C
}
s06
s07() {
  false && cat <<A
S07 and-skipped
A
  true || cat <<B
S07 or-skipped
B
  cat <<C
S07 after
C
}
s07
s08() {
  inner() {
    cat <<A
S08 inner $1
A
  }
  cat <<B
S08 outer
B
  inner one
  inner two
}
s08
r09() { cat <<A
S09 old
A
}
r09
r09() { cat <<A
S09 new
A
}
r09
s10() {
  if false; then
    cat <<A; cat <<B
S10 a
A
S10 b
B
  fi
  cat <<C
S10 after
C
}
s10
s11() {
  if true; then
    v=$(cat <<A
S11 sub
A
)
    echo "$v"
  fi
}
s11
for i in 1 2; do
  cat <<A | tr a-z A-Z
s12 pass $i
A
done
s13() {
  if false; then
    cat <<'A'
S13 $nope
A
  fi
  cat <<B
S13 after
B
}
s13
s14() {
  if false; then
	cat <<-A
	S14 skipped
	A
  fi
  cat <<B
S14 after
B
}
s14
cat <<A &
S15 bg
A
wait
while read l; do
  cat <<A
S16 got $l
A
done <<B
one
two
B
f18=${TMPDIR:-/tmp}/s18.$$
cat > "$f18" <<'SRC'
s18() {
  if false; then
    cat <<A
S18 skipped
A
  fi
  cat <<B
S18 sourced
B
}
SRC
. "$f18"
s18
rm -f "$f18"
cat <<E0 >/dev/null
line 0
E0
cat <<E1 >/dev/null
line 1
E1
cat <<E2 >/dev/null
line 2
E2
cat <<E3 >/dev/null
line 3
E3
cat <<E4 >/dev/null
line 4
E4
cat <<E5 >/dev/null
line 5
E5
cat <<E6 >/dev/null
line 6
E6
cat <<E7 >/dev/null
line 7
E7
cat <<E8 >/dev/null
line 8
E8
cat <<E9 >/dev/null
line 9
E9
cat <<E10 >/dev/null
line 10
E10
cat <<E11 >/dev/null
line 11
E11
cat <<E12 >/dev/null
line 12
E12
cat <<E13 >/dev/null
line 13
E13
cat <<E14 >/dev/null
line 14
E14
cat <<E15 >/dev/null
line 15
E15
cat <<E16 >/dev/null
line 16
E16
cat <<E17 >/dev/null
line 17
E17
cat <<E18 >/dev/null
line 18
E18
cat <<E19 >/dev/null
line 19
E19
cat <<E20 >/dev/null
line 20
E20
cat <<E21 >/dev/null
line 21
E21
cat <<E22 >/dev/null
line 22
E22
cat <<E23 >/dev/null
line 23
E23
cat <<E24 >/dev/null
line 24
E24
cat <<E25 >/dev/null
line 25
E25
cat <<E26 >/dev/null
line 26
E26
cat <<E27 >/dev/null
line 27
E27
cat <<E28 >/dev/null
line 28
E28
cat <<E29 >/dev/null
line 29
E29
cat <<E30 >/dev/null
line 30
E30
cat <<E31 >/dev/null
line 31
E31
cat <<E32 >/dev/null
line 32
E32
cat <<E33 >/dev/null
line 33
E33
cat <<E34 >/dev/null
line 34
E34
cat <<E35 >/dev/null
line 35
E35
cat <<E36 >/dev/null
line 36
E36
cat <<E37 >/dev/null
line 37
E37
cat <<E38 >/dev/null
line 38
E38
cat <<E39 >/dev/null
line 39
E39
cat <<E40 >/dev/null
line 40
E40
cat <<E41 >/dev/null
line 41
E41
cat <<E42 >/dev/null
line 42
E42
cat <<E43 >/dev/null
line 43
E43
cat <<E44 >/dev/null
line 44
E44
cat <<E45 >/dev/null
line 45
E45
cat <<E46 >/dev/null
line 46
E46
cat <<E47 >/dev/null
line 47
E47
cat <<E48 >/dev/null
line 48
E48
cat <<E49 >/dev/null
line 49
E49
cat <<E50 >/dev/null
line 50
E50
cat <<E51 >/dev/null
line 51
E51
cat <<E52 >/dev/null
line 52
E52
cat <<E53 >/dev/null
line 53
E53
cat <<E54 >/dev/null
line 54
E54
cat <<E55 >/dev/null
line 55
E55
cat <<E56 >/dev/null
line 56
E56
cat <<E57 >/dev/null
line 57
E57
cat <<E58 >/dev/null
line 58
E58
cat <<E59 >/dev/null
line 59
E59
cat <<E60 >/dev/null
line 60
E60
cat <<E61 >/dev/null
line 61
E61
cat <<E62 >/dev/null
line 62
E62
cat <<E63 >/dev/null
line 63
E63
cat <<E64 >/dev/null
line 64
E64
cat <<E65 >/dev/null
line 65
E65
cat <<E66 >/dev/null
line 66
E66
cat <<E67 >/dev/null
line 67
E67
cat <<E68 >/dev/null
line 68
E68
cat <<E69 >/dev/null
line 69
E69
cat <<E70 >/dev/null
line 70
E70
cat <<E71 >/dev/null
line 71
E71
cat <<E72 >/dev/null
line 72
E72
cat <<E73 >/dev/null
line 73
E73
cat <<E74 >/dev/null
line 74
E74
cat <<E75 >/dev/null
line 75
E75
cat <<E76 >/dev/null
line 76
E76
cat <<E77 >/dev/null
line 77
E77
cat <<E78 >/dev/null
line 78
E78
cat <<E79 >/dev/null
line 79
E79
cat <<E80 >/dev/null
line 80
E80
cat <<E81 >/dev/null
line 81
E81
cat <<E82 >/dev/null
line 82
E82
cat <<E83 >/dev/null
line 83
E83
cat <<E84 >/dev/null
line 84
E84
cat <<E85 >/dev/null
line 85
E85
cat <<E86 >/dev/null
line 86
E86
cat <<E87 >/dev/null
line 87
E87
cat <<E88 >/dev/null
line 88
E88
cat <<E89 >/dev/null
line 89
E89
cat <<E90 >/dev/null
line 90
E90
cat <<E91 >/dev/null
line 91
E91
cat <<E92 >/dev/null
line 92
E92
cat <<E93 >/dev/null
line 93
E93
cat <<E94 >/dev/null
line 94
E94
cat <<E95 >/dev/null
line 95
E95
cat <<E96 >/dev/null
line 96
E96
cat <<E97 >/dev/null
line 97
E97
cat <<E98 >/dev/null
line 98
E98
cat <<E99 >/dev/null
line 99
E99
cat <<E100 >/dev/null
line 100
E100
cat <<E101 >/dev/null
line 101
E101
cat <<E102 >/dev/null
line 102
E102
cat <<E103 >/dev/null
line 103
E103
cat <<E104 >/dev/null
line 104
E104
cat <<E105 >/dev/null
line 105
E105
cat <<E106 >/dev/null
line 106
E106
cat <<E107 >/dev/null
line 107
E107
cat <<E108 >/dev/null
line 108
E108
cat <<E109 >/dev/null
line 109
E109
cat <<E110 >/dev/null
line 110
E110
cat <<E111 >/dev/null
line 111
E111
cat <<E112 >/dev/null
line 112
E112
cat <<E113 >/dev/null
line 113
E113
cat <<E114 >/dev/null
line 114
E114
cat <<E115 >/dev/null
line 115
E115
cat <<E116 >/dev/null
line 116
E116
cat <<E117 >/dev/null
line 117
E117
cat <<E118 >/dev/null
line 118
E118
cat <<E119 >/dev/null
line 119
E119
cat <<E120 >/dev/null
line 120
E120
cat <<E121 >/dev/null
line 121
E121
cat <<E122 >/dev/null
line 122
E122
cat <<E123 >/dev/null
line 123
E123
cat <<E124 >/dev/null
line 124
E124
cat <<E125 >/dev/null
line 125
E125
cat <<E126 >/dev/null
line 126
E126
cat <<E127 >/dev/null
line 127
E127
cat <<E128 >/dev/null
line 128
E128
cat <<E129 >/dev/null
line 129
E129
cat <<E130 >/dev/null
line 130
E130
cat <<E131 >/dev/null
line 131
E131
cat <<E132 >/dev/null
line 132
E132
cat <<E133 >/dev/null
line 133
E133
cat <<E134 >/dev/null
line 134
E134
cat <<E135 >/dev/null
line 135
E135
cat <<E136 >/dev/null
line 136
E136
cat <<E137 >/dev/null
line 137
E137
cat <<E138 >/dev/null
line 138
E138
cat <<E139 >/dev/null
line 139
E139
cat <<E140 >/dev/null
line 140
E140
cat <<E141 >/dev/null
line 141
E141
cat <<E142 >/dev/null
line 142
E142
cat <<E143 >/dev/null
line 143
E143
cat <<E144 >/dev/null
line 144
E144
cat <<E145 >/dev/null
line 145
E145
cat <<E146 >/dev/null
line 146
E146
cat <<E147 >/dev/null
line 147
E147
cat <<E148 >/dev/null
line 148
E148
cat <<E149 >/dev/null
line 149
E149
cat <<E150 >/dev/null
line 150
E150
cat <<E151 >/dev/null
line 151
E151
cat <<E152 >/dev/null
line 152
E152
cat <<E153 >/dev/null
line 153
E153
cat <<E154 >/dev/null
line 154
E154
cat <<E155 >/dev/null
line 155
E155
cat <<E156 >/dev/null
line 156
E156
cat <<E157 >/dev/null
line 157
E157
cat <<E158 >/dev/null
line 158
E158
cat <<E159 >/dev/null
line 159
E159
cat <<E160 >/dev/null
line 160
E160
cat <<E161 >/dev/null
line 161
E161
cat <<E162 >/dev/null
line 162
E162
cat <<E163 >/dev/null
line 163
E163
cat <<E164 >/dev/null
line 164
E164
cat <<E165 >/dev/null
line 165
E165
cat <<E166 >/dev/null
line 166
E166
cat <<E167 >/dev/null
line 167
E167
cat <<E168 >/dev/null
line 168
E168
cat <<E169 >/dev/null
line 169
E169
cat <<E170 >/dev/null
line 170
E170
cat <<E171 >/dev/null
line 171
E171
cat <<E172 >/dev/null
line 172
E172
cat <<E173 >/dev/null
line 173
E173
cat <<E174 >/dev/null
line 174
E174
cat <<E175 >/dev/null
line 175
E175
cat <<E176 >/dev/null
line 176
E176
cat <<E177 >/dev/null
line 177
E177
cat <<E178 >/dev/null
line 178
E178
cat <<E179 >/dev/null
line 179
E179
cat <<E180 >/dev/null
line 180
E180
cat <<E181 >/dev/null
line 181
E181
cat <<E182 >/dev/null
line 182
E182
cat <<E183 >/dev/null
line 183
E183
cat <<E184 >/dev/null
line 184
E184
cat <<E185 >/dev/null
line 185
E185
cat <<E186 >/dev/null
line 186
E186
cat <<E187 >/dev/null
line 187
E187
cat <<E188 >/dev/null
line 188
E188
cat <<E189 >/dev/null
line 189
E189
cat <<E190 >/dev/null
line 190
E190
cat <<E191 >/dev/null
line 191
E191
cat <<E192 >/dev/null
line 192
E192
cat <<E193 >/dev/null
line 193
E193
cat <<E194 >/dev/null
line 194
E194
cat <<E195 >/dev/null
line 195
E195
cat <<E196 >/dev/null
line 196
E196
cat <<E197 >/dev/null
line 197
E197
cat <<E198 >/dev/null
line 198
E198
cat <<E199 >/dev/null
line 199
E199
cat <<E200 >/dev/null
line 200
E200
cat <<E201 >/dev/null
line 201
E201
cat <<E202 >/dev/null
line 202
E202
cat <<E203 >/dev/null
line 203
E203
cat <<E204 >/dev/null
line 204
E204
cat <<E205 >/dev/null
line 205
E205
cat <<E206 >/dev/null
line 206
E206
cat <<E207 >/dev/null
line 207
E207
cat <<E208 >/dev/null
line 208
E208
cat <<E209 >/dev/null
line 209
E209
cat <<E210 >/dev/null
line 210
E210
cat <<E211 >/dev/null
line 211
E211
cat <<E212 >/dev/null
line 212
E212
cat <<E213 >/dev/null
line 213
E213
cat <<E214 >/dev/null
line 214
E214
cat <<E215 >/dev/null
line 215
E215
cat <<E216 >/dev/null
line 216
E216
cat <<E217 >/dev/null
line 217
E217
cat <<E218 >/dev/null
line 218
E218
cat <<E219 >/dev/null
line 219
E219
cat <<E220 >/dev/null
line 220
E220
cat <<E221 >/dev/null
line 221
E221
cat <<E222 >/dev/null
line 222
E222
cat <<E223 >/dev/null
line 223
E223
cat <<E224 >/dev/null
line 224
E224
cat <<E225 >/dev/null
line 225
E225
cat <<E226 >/dev/null
line 226
E226
cat <<E227 >/dev/null
line 227
E227
cat <<E228 >/dev/null
line 228
E228
cat <<E229 >/dev/null
line 229
E229
cat <<E230 >/dev/null
line 230
E230
cat <<E231 >/dev/null
line 231
E231
cat <<E232 >/dev/null
line 232
E232
cat <<E233 >/dev/null
line 233
E233
cat <<E234 >/dev/null
line 234
E234
cat <<E235 >/dev/null
line 235
E235
cat <<E236 >/dev/null
line 236
E236
cat <<E237 >/dev/null
line 237
E237
cat <<E238 >/dev/null
line 238
E238
cat <<E239 >/dev/null
line 239
E239
cat <<E240 >/dev/null
line 240
E240
cat <<E241 >/dev/null
line 241
E241
cat <<E242 >/dev/null
line 242
E242
cat <<E243 >/dev/null
line 243
E243
cat <<E244 >/dev/null
line 244
E244
cat <<E245 >/dev/null
line 245
E245
cat <<E246 >/dev/null
line 246
E246
cat <<E247 >/dev/null
line 247
E247
cat <<E248 >/dev/null
line 248
E248
cat <<E249 >/dev/null
line 249
E249
cat <<E250 >/dev/null
line 250
E250
cat <<E251 >/dev/null
line 251
E251
cat <<E252 >/dev/null
line 252
E252
cat <<E253 >/dev/null
line 253
E253
cat <<E254 >/dev/null
line 254
E254
cat <<E255 >/dev/null
line 255
E255
cat <<E256 >/dev/null
line 256
E256
cat <<E257 >/dev/null
line 257
E257
cat <<E258 >/dev/null
line 258
E258
cat <<E259 >/dev/null
line 259
E259
cat <<E260 >/dev/null
line 260
E260
cat <<E261 >/dev/null
line 261
E261
cat <<E262 >/dev/null
line 262
E262
cat <<E263 >/dev/null
line 263
E263
cat <<E264 >/dev/null
line 264
E264
cat <<E265 >/dev/null
line 265
E265
cat <<E266 >/dev/null
line 266
E266
cat <<E267 >/dev/null
line 267
E267
cat <<E268 >/dev/null
line 268
E268
cat <<E269 >/dev/null
line 269
E269
cat <<E270 >/dev/null
line 270
E270
cat <<E271 >/dev/null
line 271
E271
cat <<E272 >/dev/null
line 272
E272
cat <<E273 >/dev/null
line 273
E273
cat <<E274 >/dev/null
line 274
E274
cat <<E275 >/dev/null
line 275
E275
cat <<E276 >/dev/null
line 276
E276
cat <<E277 >/dev/null
line 277
E277
cat <<E278 >/dev/null
line 278
E278
cat <<E279 >/dev/null
line 279
E279
cat <<E280 >/dev/null
line 280
E280
cat <<E281 >/dev/null
line 281
E281
cat <<E282 >/dev/null
line 282
E282
cat <<E283 >/dev/null
line 283
E283
cat <<E284 >/dev/null
line 284
E284
cat <<E285 >/dev/null
line 285
E285
cat <<E286 >/dev/null
line 286
E286
cat <<E287 >/dev/null
line 287
E287
cat <<E288 >/dev/null
line 288
E288
cat <<E289 >/dev/null
line 289
E289
cat <<E290 >/dev/null
line 290
E290
cat <<E291 >/dev/null
line 291
E291
cat <<E292 >/dev/null
line 292
E292
cat <<E293 >/dev/null
line 293
E293
cat <<E294 >/dev/null
line 294
E294
cat <<E295 >/dev/null
line 295
E295
cat <<E296 >/dev/null
line 296
E296
cat <<E297 >/dev/null
line 297
E297
cat <<E298 >/dev/null
line 298
E298
cat <<E299 >/dev/null
line 299
E299
echo S19 many-done
for i in 1 2; do
  if [ $i = 1 ]; then
    cat <<A
S20 one
A
  else
    cat <<B
S20 two
B
  fi
done
f21() {
  cat <<A
x
A
}
echo S21 tags=$(declare -f f21 | grep -c $'\x1e')
echo done
