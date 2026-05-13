const express = require('express');
const fetch = require('node-fetch');
const app = express();

app.use(express.json());

app.post('/genius', async (req, res) => {
  try {
    const response = await fetch('https://api.geniusenv.com/api/v1.1.1/pollutracker/data/create/', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(req.body)
    });
    const data = await response.text();
    res.status(response.status).send(data);
  } catch (error) {
    console.error(error);
    res.status(500).send('Proxy error');
  }
});

const PORT = process.env.PORT || 3000;
app.listen(PORT, () => console.log(`Proxy running on port ${PORT}`));
