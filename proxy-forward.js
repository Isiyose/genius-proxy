// proxy-forward.js
const express = require('express');
const axios = require('axios');
const app = express();
app.use(express.json());

app.post('/genius', async (req, res) => {
  try {
    const response = await axios.post(
      'https://genius-proxy-82ua.onrender.com/genius',
      req.body,
      { headers: { 'Content-Type': 'application/json' } }
    );
    res.status(response.status).send(response.data);
  } catch (err) {
    console.error('Forward error:', err.message);
    res.status(500).send({ error: 'Forward failed' });
  }
});

const port = process.env.PORT || 10000;
app.listen(port, '0.0.0.0', () => console.log(`HTTP forwarder running on ${port}`));
