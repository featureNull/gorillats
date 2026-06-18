use gorillats::{
    Compressor, Decompressor, FloatCompressor, FloatDecompressor,
};

#[test]
fn roundtrip_f64() {
    let t0 = 1_600_000_000i64;
    let points: Vec<(i64, f64)> = vec![
        (1_600_000_001, 1.0),
        (1_600_000_002, 1.0),  // unchanged value -> single bit
        (1_600_000_062, 2.5),  // larger gap + new value
        (1_600_000_122, 2.5),
        (1_600_000_182, -0.0),
    ];

    let mut c = Compressor::new(t0, 4096);
    for &(ts, v) in &points {
        c.append(ts, v).unwrap();
    }
    let bytes = c.finish();

    let out: Vec<(i64, f64)> = Decompressor::new(bytes).unwrap().into_iter().collect();
    assert_eq!(out.len(), points.len());
    for ((ats, av), (ets, ev)) in out.iter().zip(points.iter()) {
        assert_eq!(ats, ets);
        assert_eq!(av.to_bits(), ev.to_bits());
    }
}

#[test]
fn roundtrip_constant_series() {
    let mut c = Compressor::new(0, 1024);
    for i in 0..100 {
        c.append(i, 42.0).unwrap();
    }
    let bytes = c.finish();
    let out: Vec<(i64, f64)> = Decompressor::new(bytes).unwrap().into_iter().collect();
    assert_eq!(out.len(), 100);
    assert!(out.iter().all(|&(_, v)| v == 42.0));
}

#[test]
fn roundtrip_special_values() {
    let specials = [f64::NAN, f64::INFINITY, f64::NEG_INFINITY, 0.0, -0.0];
    let mut c = Compressor::new(0, 1024);
    for (i, &v) in specials.iter().enumerate() {
        c.append(i as i64, v).unwrap();
    }
    let bytes = c.finish();
    let out: Vec<(i64, f64)> = Decompressor::new(bytes).unwrap().into_iter().collect();
    for (got, &exp) in out.iter().zip(specials.iter()) {
        assert_eq!(got.1.to_bits(), exp.to_bits());
    }
}

#[test]
fn roundtrip_f32() {
    let mut c = FloatCompressor::new(0, 1024);
    let vals = [1.0f32, 1.0, 3.25, 3.25, -7.5];
    for (i, &v) in vals.iter().enumerate() {
        c.append(i as i64, v).unwrap();
    }
    let bytes = c.finish();
    let out: Vec<(i64, f32)> =
        FloatDecompressor::new(bytes).unwrap().into_iter().collect();
    for (got, &exp) in out.iter().zip(vals.iter()) {
        assert_eq!(got.1.to_bits(), exp.to_bits());
    }
}

#[test]
fn single_point() {
    let mut c = Compressor::new(10, 64);
    c.append(11, 3.14).unwrap();
    let bytes = c.finish();
    let out: Vec<(i64, f64)> = Decompressor::new(bytes).unwrap().into_iter().collect();
    assert_eq!(out, vec![(11, 3.14)]);
}

#[test]
fn wrong_value_type_rejected() {
    let mut c = Compressor::new(0, 64);
    c.append(1, 1.0).unwrap();
    let bytes = c.finish();
    assert!(FloatDecompressor::new(bytes).is_err());
}
