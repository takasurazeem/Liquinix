// Reescreve a seção .rsrc de um PE só de recursos (ex.: imageres.dll.mun), trocando grupos de ícone.
// Necessário porque UpdateResource recusa arquivos com recurso "MUI" (erro 50).
using System; using System.IO; using System.Collections.Generic; using System.Linq; using System.Text; using System.Runtime.InteropServices;
public static class Rsrc {
  class Key : IComparable<Key> {
    public string S; public int Id;
    public int CompareTo(Key o) {
      if (S != null && o.S == null) return -1; if (S == null && o.S != null) return 1;
      if (S != null) return string.CompareOrdinal(S.ToUpperInvariant(), o.S.ToUpperInvariant());
      return Id.CompareTo(o.Id);
    }
    public override bool Equals(object x) { var o = x as Key; return o != null && o.S == S && o.Id == Id; }
    public override int GetHashCode() { return S != null ? S.GetHashCode() : Id; }
  }
  class Leaf { public byte[] Data; public uint CodePage; }
  // tipo -> nome -> língua -> dado
  static SortedDictionary<Key, SortedDictionary<Key, SortedDictionary<Key, Leaf>>> tree;

  [DllImport("imagehlp.dll", CharSet = CharSet.Unicode)] static extern uint MapFileAndCheckSumW(string f, out uint hdr, out uint sum);

  static uint U32(byte[] b, int o) { return BitConverter.ToUInt32(b, o); }
  static ushort U16(byte[] b, int o) { return BitConverter.ToUInt16(b, o); }

  static Key ReadKey(byte[] b, int rs, uint name) {
    if ((name & 0x80000000) != 0) { int o = rs + (int)(name & 0x7FFFFFFF); int len = U16(b, o); return new Key { S = Encoding.Unicode.GetString(b, o + 2, len * 2) }; }
    return new Key { Id = (int)name };
  }
  static IEnumerable<KeyValuePair<Key, uint>> Entries(byte[] b, int rs, int dir) {
    int n = U16(b, dir + 12) + U16(b, dir + 14);
    for (int i = 0; i < n; i++) { int e = dir + 16 + 8 * i; yield return new KeyValuePair<Key, uint>(ReadKey(b, rs, U32(b, e)), U32(b, e + 4)); }
  }

  public static string Patch(string file, int[] ids, string[] icos) {
    byte[] b = File.ReadAllBytes(file);
    int pe = (int)U32(b, 0x3c); int nsec = U16(b, pe + 6); int optSize = U16(b, pe + 20); int opt = pe + 24;
    bool pe64 = U16(b, opt) == 0x20B;
    int ddir = opt + (pe64 ? 112 : 96);
    uint secAlign = U32(b, opt + 32), fileAlign = U32(b, opt + 36);
    uint rsrcRva = U32(b, ddir + 16);
    int sh = -1;
    for (int i = 0; i < nsec; i++) { int o = opt + optSize + 40 * i; uint va = U32(b, o + 12), vs = Math.Max(U32(b, o + 8), U32(b, o + 16)); if (rsrcRva >= va && rsrcRva < va + vs) sh = o; }
    if (sh < 0) return "seção .rsrc não encontrada";
    if (sh != opt + optSize + 40 * (nsec - 1)) return ".rsrc não é a última seção";
    uint secVa = U32(b, sh + 12); int secRaw = (int)U32(b, sh + 20);
    if (rsrcRva != secVa) return ".rsrc não começa no início da seção";
    int rs = secRaw;
    // lê a árvore
    tree = new SortedDictionary<Key, SortedDictionary<Key, SortedDictionary<Key, Leaf>>>();
    foreach (var t in Entries(b, rs, rs)) {
      var names = new SortedDictionary<Key, SortedDictionary<Key, Leaf>>(); tree[t.Key] = names;
      foreach (var n in Entries(b, rs, rs + (int)(t.Value & 0x7FFFFFFF))) {
        var langs = new SortedDictionary<Key, Leaf>(); names[n.Key] = langs;
        foreach (var l in Entries(b, rs, rs + (int)(n.Value & 0x7FFFFFFF))) {
          int de = rs + (int)l.Value; uint rva = U32(b, de), size = U32(b, de + 4);
          byte[] d = new byte[size]; Array.Copy(b, (int)(rva - secVa) + secRaw, d, 0, size);
          langs[l.Key] = new Leaf { Data = d, CodePage = U32(b, de + 8) };
        }
      }
    }
    // troca os grupos
    var kIcon = new Key { Id = 3 }; var kGroup = new Key { Id = 14 };
    var icons = tree[kIcon]; var groups = tree[kGroup];
    int next = Math.Max(icons.Keys.Where(k => k.S == null).Select(k => k.Id).DefaultIfEmpty(0).Max() + 1, 40000);
    int trocados = 0;
    for (int k = 0; k < ids.Length; k++) {
      var gk = new Key { Id = ids[k] };
      List<Key> langKeys = groups.ContainsKey(gk) ? groups[gk].Keys.ToList() : new List<Key> { new Key { Id = 0x409 } };
      byte[] ico = File.ReadAllBytes(icos[k]);
      int count = U16(ico, 4);
      var ms = new MemoryStream(); var w = new BinaryWriter(ms);
      w.Write((ushort)0); w.Write((ushort)1); w.Write((ushort)count);
      for (int i = 0; i < count; i++) {
        int o = 6 + 16 * i;
        ushort planes = U16(ico, o + 4), bpp = U16(ico, o + 6); uint size = U32(ico, o + 8), off = U32(ico, o + 12);
        byte[] img = new byte[size]; Array.Copy(ico, off, img, 0, size);
        if (next > 65535) return "sem IDs livres";
        var nk = new Key { Id = next++ };
        var ld = new SortedDictionary<Key, Leaf>();
        foreach (var lk in langKeys) ld[new Key { Id = lk.Id }] = new Leaf { Data = img };
        icons[nk] = ld;
        w.Write(ico[o]); w.Write(ico[o + 1]); w.Write(ico[o + 2]); w.Write((byte)0);
        w.Write(planes == 0 ? (ushort)1 : planes); w.Write(bpp == 0 ? (ushort)32 : bpp); w.Write(size); w.Write((ushort)nk.Id);
      }
      var gd = new SortedDictionary<Key, Leaf>();
      foreach (var lk in langKeys) gd[new Key { Id = lk.Id }] = new Leaf { Data = ms.ToArray() };
      groups[gk] = gd; trocados++;
    }
    // serializa a nova .rsrc
    byte[] sec = Build(secVa);
    uint rawSize = Align((uint)sec.Length, fileAlign);
    byte[] nb = new byte[secRaw + rawSize];
    Array.Copy(b, nb, secRaw);
    Array.Copy(sec, 0, nb, secRaw, sec.Length);
    Put(nb, sh + 8, (uint)sec.Length);          // VirtualSize
    Put(nb, sh + 16, rawSize);                  // SizeOfRawData
    Put(nb, ddir + 20, (uint)sec.Length);       // tamanho do diretório de recursos
    Put(nb, ddir + 32, 0); Put(nb, ddir + 36, 0); // sem certificado
    Put(nb, opt + 56, Align(secVa + (uint)sec.Length, secAlign)); // SizeOfImage
    Put(nb, opt + 64, 0);
    File.WriteAllBytes(file, nb);
    uint hdr, sum; if (MapFileAndCheckSumW(file, out hdr, out sum) == 0) { byte[] c = File.ReadAllBytes(file); Put(c, opt + 64, sum); File.WriteAllBytes(file, c); }
    return "ok (" + trocados + " grupos, " + b.Length + " -> " + nb.Length + " bytes)";
  }
  static uint Align(uint v, uint a) { return (v + a - 1) / a * a; }
  static void Put(byte[] b, int o, uint v) { BitConverter.GetBytes(v).CopyTo(b, o); }

  static byte[] Build(uint secVa) {
    // 1) diretórios (tipos, nomes, línguas) em largura; 2) strings; 3) data entries; 4) dados
    int dirSize = 0;
    Func<int, int> D = n => 16 + 8 * n;
    dirSize += D(tree.Count);
    foreach (var t in tree) { dirSize += D(t.Value.Count); foreach (var n in t.Value) dirSize += D(n.Value.Count); }
    var strs = new List<string>();
    foreach (var t in tree) { if (t.Key.S != null) strs.Add(t.Key.S); foreach (var n in t.Value) { if (n.Key.S != null) strs.Add(n.Key.S); foreach (var l in n.Value) if (l.Key.S != null) strs.Add(l.Key.S); } }
    var strOff = new Dictionary<string, int>(); int p = dirSize;
    foreach (var s in strs.Distinct()) { strOff[s] = p; p += 2 + s.Length * 2; }
    p = (p + 7) & ~7;
    int leafCount = tree.Sum(t => t.Value.Sum(n => n.Value.Count));
    int deStart = p; p += 16 * leafCount; p = (p + 7) & ~7;
    int dataStart = p;
    long total = dataStart; foreach (var t in tree) foreach (var n in t.Value) foreach (var l in n.Value) total += (l.Value.Data.Length + 7) & ~7;
    byte[] o = new byte[total];
    foreach (var s in strOff) { BitConverter.GetBytes((ushort)s.Key.Length).CopyTo(o, s.Value); Encoding.Unicode.GetBytes(s.Key).CopyTo(o, s.Value + 2); }
    int dirPos = 0, dePos = deStart, dataPos = dataStart;
    // posições dos diretórios: tipos, depois todos os de nome, depois todos os de língua
    int typeDir = dirPos; dirPos += D(tree.Count);
    var nameDirs = new List<int>(); foreach (var t in tree) { nameDirs.Add(dirPos); dirPos += D(t.Value.Count); }
    var langDirs = new List<int>(); foreach (var t in tree) foreach (var n in t.Value) { langDirs.Add(dirPos); dirPos += D(n.Value.Count); }
    Action<int, IEnumerable<Key>, int> Header = (at, keys, unused) => {
      int named = keys.Count(k => k.S != null), idc = keys.Count(k => k.S == null);
      BitConverter.GetBytes((ushort)named).CopyTo(o, at + 12); BitConverter.GetBytes((ushort)idc).CopyTo(o, at + 14);
    };
    Func<Key, uint> NameField = k => k.S != null ? (uint)strOff[k.S] | 0x80000000u : (uint)k.Id;
    Header(typeDir, tree.Keys, 0);
    int ti = 0, li = 0;
    foreach (var t in tree) {
      int e = typeDir + 16 + 8 * ti;
      Put(o, e, NameField(t.Key)); Put(o, e + 4, (uint)nameDirs[ti] | 0x80000000u);
      int nd = nameDirs[ti]; Header(nd, t.Value.Keys, 0);
      int ni = 0;
      foreach (var n in t.Value) {
        int e2 = nd + 16 + 8 * ni;
        int ld = langDirs[li++];
        Put(o, e2, NameField(n.Key)); Put(o, e2 + 4, (uint)ld | 0x80000000u);
        Header(ld, n.Value.Keys, 0);
        int lj = 0;
        foreach (var l in n.Value) {
          int e3 = ld + 16 + 8 * lj;
          Put(o, e3, NameField(l.Key)); Put(o, e3 + 4, (uint)dePos);
          Put(o, dePos, secVa + (uint)dataPos); Put(o, dePos + 4, (uint)l.Value.Data.Length); Put(o, dePos + 8, l.Value.CodePage);
          l.Value.Data.CopyTo(o, dataPos);
          dePos += 16; dataPos += (l.Value.Data.Length + 7) & ~7; lj++;
        }
        ni++;
      }
      ti++;
    }
    return o;
  }
}
