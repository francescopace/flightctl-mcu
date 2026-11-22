// SPDX-License-Identifier: Apache-2.0
// Original kernels copyright 2018 Embedded Microprocessor Benchmark Consortium (EEMBC).
// Local translation of https://github.com/eembc/coremark/tree/1f483d5
// Apache License 2.0: https://www.apache.org/licenses/LICENSE-2.0
// This experimental Toit translation uses the reference 2,000-byte workload.
BLOCK-SIZE ::= 2000 / 3
PATTERNS ::= [["5012","1234","-874","+122"], ["35.54400",".1234500","-110.700","+0.64400"],
              ["5.500e+3","-.123e-2","-87e+832","+0.6e-12"], ["T0.3e-1F","-T.T++Tq","1T3.4e4z","34.0e-T^"]]

crc16 value/int crc/int -> int:
  crc &= 0xffff
  16.repeat:
    bit := (crc ^ value) & 1
    value >>= 1
    crc = (crc >> 1) ^ (bit == 1 ? 0xa001 : 0)
  return crc

crc32 value/int crc/int -> int:
  return crc16 (value >> 16) (crc16 value crc)

signed16 value/int -> int:
  return ((value & 0xffff) ^ 0x8000) - 0x8000

class Matrix:
  n/int := 0
  a/List := ?
  b/List := ?
  c/List := ?

  constructor seed/int:
    while (n + 1) * (n + 1) * 8 < BLOCK-SIZE: n++
    cells := n * n
    a = List cells 0
    b = List cells 0
    c = List cells 0
    seed = seed == 0 ? 1 : seed
    cells.repeat: |i|
      order := i + 1
      seed = (order * seed) & 0xffff
      b[i] = signed16 (seed + order)
      a[i] = (b[i] + order) & 0xff

  sum clip/int -> int:
    acc := 0
    prev := 0
    score := 0
    c.do: |cur|
      acc += cur
      if acc > clip:
        score += 10
        acc = 0
      else if cur > prev:
        score++
      prev = cur
    return score

  run val/int crc/int -> int:
    clip := signed16 (0xf000 | val)
    a.size.repeat: |i| a[i] += val
    a.size.repeat: |i| c[i] = a[i] * val
    result := crc16 (sum clip) 0
    n.repeat: |i|
      c[i] = 0
      n.repeat: |j| c[i] += a[i*n+j] * b[j]
    result = crc16 (sum clip) result
    n.repeat: |i|
      n.repeat: |j|
        cell := i*n+j
        c[cell] = 0
        n.repeat: |k| c[cell] += a[i*n+k] * b[k*n+j]
    result = crc16 (sum clip) result
    n.repeat: |i|
      n.repeat: |j|
        cell := i*n+j
        c[cell] = 0
        n.repeat: |k|
          product := a[i*n+k] * b[k*n+j]
          c[cell] += ((product >> 2) & 15) * ((product >> 5) & 127)
    result = crc16 (sum clip) result
    a.size.repeat: |i| a[i] -= val
    return crc16 result crc

START ::= 0
INVALID ::= 1
SIGN ::= 2
EXP-MARK ::= 3
INTEGER ::= 4
FRACTION ::= 5
EXP-SIGN ::= 6
SCIENTIFIC ::= 7

class Scanner:
  bytes/ByteArray := ByteArray BLOCK-SIZE
  constructor seed/int:
    end := 0
    while true:
      seed++
      kind := seed & 7
      row := kind < 3 ? 0 : kind < 5 ? 1 : kind < 7 ? 2 : 3
      token := PATTERNS[row][(seed >> 3) & 3]
      if end + token.size + 1 >= BLOCK-SIZE - 1: break
      token.size.repeat: |i| bytes[end++] = token.byte-at i
      bytes[end++] = 44

  scan ends/List visits/List:
    pos := 0
    while bytes[pos] != 0:
      state := START
      while bytes[pos] != 0 and state != INVALID:
        ch := bytes[pos++]
        if ch == 44: break
        digit := ch >= 48 and ch <= 57
        sign := ch == 43 or ch == 45
        previous := state
        if state == START or state == SIGN:
          if digit: state = INTEGER
          else if ch == 46: state = FRACTION
          else if sign and state == START: state = SIGN
          else:
            state = INVALID
            if previous == START: visits[INVALID]++
        else if state == INTEGER:
          if ch == 46: state = FRACTION
          else if not digit: state = INVALID
        else if state == FRACTION:
          if ch == 69 or ch == 101: state = EXP-MARK
          else if not digit: state = INVALID
        else if state == EXP-MARK:
          state = sign ? EXP-SIGN : INVALID
        else if state == EXP-SIGN:
          state = digit ? SCIENTIFIC : INVALID
        else if not digit:
          state = INVALID
        if previous != state:
          visits[previous == SCIENTIFIC ? INVALID : previous]++
      ends[state]++

  corrupt seed/int step/int:
    for i := 0; i < bytes.size; i += step:
      if bytes[i] != 44: bytes[i] ^= seed & 0xff

  run seed/int step/int crc/int -> int:
    ends := List 8 0
    visits := List 8 0
    scan ends visits
    corrupt seed step
    scan ends visits
    corrupt seed step
    8.repeat: |i|
      crc = crc32 ends[i] crc
      crc = crc32 visits[i] crc
    return crc

class Data:
  value/int := ?
  idx/int := ?
  constructor .value .idx:

class Node:
  next/Node? := ?
  data/Data := ?
  constructor .data .next:

find-node node/Node? idx/int value/int -> Node?:
  while node:
    if idx >= 0 ? node.data.idx == idx : (node.data.value & 255) == value: break
    node = node.next
  return node

reverse node/Node? -> Node:
  prev := null
  while node:
    next := node.next
    node.next = prev
    prev = node
    node = next
  return prev

class Workload:
  seed/int := ?
  crc/int := 0
  list-crc/int := 0
  matrix-crc/int := 0
  state-crc/int := 0
  scanner/Scanner := ?
  matrix/Matrix := ?
  head/Node := ?

  constructor .seed:
    scanner = Scanner seed
    matrix = Matrix (seed | (seed << 16))
    head = Node (Data 0x8080 0) (Node (Data 0xffff 0x7fff) null)
    capacity := BLOCK-SIZE / 20 - 2
    (capacity - 3).repeat: |i|
      pattern := (((seed ^ i) & 15) << 3) | (i & 7)
      head.next = Node (Data ((pattern << 8) | pattern) 0) head.next
    node := head.next
    i := 1
    while node.next:
      pattern := i ^ seed
      node.data.idx = i < capacity / 5 ? i : ((((i + 1) & 7) << 8) | pattern) & 0x3fff
      i++
      node = node.next
    head = sort head false

  calc data/Data -> int:
    v := data.value
    if v & 0x80 != 0: return v & 127
    arg := ((v >> 3) & 15) * 17
    result := v
    if (v & 7) == 0:
      result = scanner.run seed (max 34 arg) crc
      if state-crc == 0: state-crc = result
    else if (v & 7) == 1:
      result = matrix.run arg crc
      if matrix-crc == 0: matrix-crc = result
    crc = crc16 result crc
    data.value = (v & 0xff00) | 128 | (result & 127)
    return result & 127

  compare x/Data y/Data complex/bool -> int:
    if complex:
      first := calc x
      return first - (calc y)
    x.value = (x.value & 0xff00) | ((x.value >> 8) & 255)
    y.value = (y.value & 0xff00) | ((y.value >> 8) & 255)
    return x.idx - y.idx

  sort list/Node complex/bool -> Node:
    width := 1
    while true:
      left/Node? := list
      tail := null
      merges := 0
      while left:
        merges++
        right/Node? := left
        nl := 0
        while right and nl < width:
          nl++
          right = right.next
        nr := width
        while nl > 0 or (nr > 0 and right):
          take := null
          if nl > 0 and (nr == 0 or not right or (compare left.data right.data complex) <= 0):
            take = left
            left = left.next
            nl--
          else:
            take = right
            right = right.next
            nr--
          if tail: tail.next = take
          else: list = take
          tail = take
        left = right
      tail.next = null
      if merges <= 1: return list
      width *= 2

  bench-list initial-idx/int -> int:
    idx := initial-idx
    list := head
    result := 0
    found := 0
    missed := 0
    0x66.repeat: |i|
      match := find-node list idx i
      list = reverse list
      if not match:
        missed++
        result += (list.next.data.value >> 8) & 1
      else:
        found++
        if match.data.value & 1 != 0: result += (match.data.value >> 9) & 1
        if match.next:
          move := match.next
          match.next = move.next
          move.next = list.next
          list.next = move
      if idx >= 0: idx++
    result += found * 4 - missed
    if initial-idx > 0: list = sort list true
    modified := list.next
    removed := modified.next
    tmp := modified.data
    modified.data = removed.data
    removed.data = tmp
    modified.next = removed.next
    removed.next = null
    cursor := find-node list idx (0x66 - 1)
    if not cursor: cursor = list.next
    while cursor:
      result = crc16 list.data.value result
      cursor = cursor.next
    tmp = removed.data
    removed.data = modified.data
    modified.data = tmp
    removed.next = modified.next
    modified.next = removed
    list = sort list false
    cursor = list.next
    while cursor:
      result = crc16 list.data.value result
      cursor = cursor.next
    return result

  iterate count/int:
    count.repeat: |i|
      forward := bench-list 1
      crc = crc16 forward crc
      back := bench-list -1
      crc = crc16 back crc
      if i == 0: list-crc = crc

measure seed/int count/int:
  w := Workload seed
  elapsed := Duration.of: w.iterate count
  perf := seed == 0
  if w.list-crc != (perf ? 0xe714 : 0xe3c1) or w.matrix-crc != (perf ? 0x1fd7 : 0x0747) or w.state-crc != (perf ? 0x8e3a : 0x8d84):
    throw "CRC mismatch seed=$seed list=$(w.list-crc) matrix=$(w.matrix-crc) state=$(w.state-crc)"
  print "KERNELS seed=$seed iterations=$count us=$(elapsed.in-us) final=$(w.crc)"

main:
  measure 0x3415 1
  3.repeat: measure 0 320
  print "KERNELS finished"
